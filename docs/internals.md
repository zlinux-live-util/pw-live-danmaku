# 实现细节

修改代码前先读本文。以下每条都来自实机验证，而非文档转述；其中若干条官方文档没有记载，或社区文档描述与实际行为不符。

本文与项目代码均在 LLM 辅助下编写。所有结论都经过实机复现——未经复现的推测不收录，因此每项都附有可重跑的对照数据或命令。

## 进度

**M0（协议打通）已完成。** 链路：`room_init` → WBI 签名 `getDanmuInfo` → TLS WebSocket 握手 → JSON 认证包 → brotli 解包 → `DANMU_MSG` 解析 → cairo 渲染 → PipeWire 视频节点 → 消费者落盘。

画面目前是**诊断卡片**，不是聊天面板；面板是 M1。

## 进程与线程

| 线程 | 职责 |
| --- | --- |
| 主线程 | `pwvideo::VideoNode::run()`（PipeWire 主循环），`process` 回调内只做渲染与拷贝 |
| 站点线程 | HTTP 引导 + WebSocket 读循环；把弹幕写进 `Shared`，帧线程只取快照 |

与视频节点库那条契约一致：**帧回调内绝不进行网络访问，也不分配**。`Shared::snapshot()` 在锁内做一次值拷贝，帧线程拿到的是快照而不是活引用。

心跳由**读循环的读超时驱动**（`ws.next(msg, 1000)` 每秒返回一次 Timeout），不另开定时器线程。

## 三个协议坑：都与社区文档相反

### 一、认证包用 JSON，protobuf 版本会被直接断连

绝大多数开源实现（Go / Python / Rust 的 bilibili 直播库）发送 protobuf 的 `ClientVerifyReq`。**实测会被服务器立刻断开连接。** 四种字段编号变体全部失败：

| 认证包 body | 结果 |
| --- | --- |
| protobuf，`uid` 作 bytes（wire type 2） | 建连后立即断开 |
| protobuf，`uid` 作 int64（varint） | 建连后立即断开 |
| protobuf，追加 `buvid` 为字段 7 | 建连后立即断开 |
| **JSON** | **拿到认证回包，成功** |

实测使用的 JSON body：

```json
{"uid":0,"roomid":21452505,"protover":3,"platform":"web","type":2,"key":"<token>"}
```

包头仍为 `proto=3, type=7`。**照抄社区文档去写 protobuf 会浪费很久的调试时间**，这条务必先看。

### 二、认证回包声明 proto=3，但 body 是明文 JSON

这是最容易踩的一个：拿到 `proto=3` 就去 brotli 解压，必然失败。

实测抓到的认证回包，一共 **26 字节**：

```text
00 00 00 1a  00 10  00 03  00 00 00 08  00 00 00 01  7b 22 63 6f 64 65 22 3a 30 7d
└─ totalSize=26 ─┘ hsize=16 proto=3 type=8  seq=1  └──────── {"code":0} ────────┘
```

而**同一个 proto=3 的弹幕批次是真的 brotli**。所以 `proto` 只能当提示，不能当契约。

因此 `Bili::forEachJson()` 先嗅探 body（以 `{` 或 `[` 开头即视作文档），再决定是否解压——见 `src/bili.cpp`。早前一版直接信 `proto`，症状是卡片上显示 `auth reply could not be decompressed`。

### 三、`getDanmuInfo` 会风控，四个条件缺一不可

不加签名的请求直接被拒：

```bash
curl -s 'https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id=545068'
# {"code":-352,"message":"-352","ttl":1}
```

实测逐项验证：

| 条件 | 结果 |
| --- | --- |
| 无 WBI 签名 | `code:-352` |
| WBI 签名，但无 buvid3 / 无 Referer / 无 Origin | `code:-352` |
| **WBI 签名 + buvid3 + `Referer` / `Origin` + 浏览器 UA** | **`code:0`，拿到 token 与 host_list** ✅ |

**buvid3 不需要登录**，`https://api.bilibili.com/x/frontend/finger/spi` 的 `data.b_3` 免费可得，等同浏览器首次访问时生成的设备 id。

WBI 签名算法（`Bili::mixinKey` / `md5Hex`）：

1. `https://api.bilibili.com/x/web-interface/nav` 取 `data.wbi_img.img_url` / `sub_url` 的**文件名部分**（去掉扩展名）。
   注意该 URL 是伪装的 token，**不要去请求它**；未登录时该接口返回 `code:-101`，但 `wbi_img` 仍然存在，所以匿名可用。
2. `raw = img_key + sub_key`（64 字符），按 64 项置换表 `MIXIN_KEY_ENC_TAB` 重排，取前 **32** 位。
3. 参数按 key 排序 → 值里剔除 `!'()*` → URL 编码 → 拼 `&wts=<unix秒>` → `md5(query + mixin_key)` 即 `w_rid`。

实测取 key 的那一步长这样（注意 `code` 是 **-101 未登录**，但 `wbi_img` 依然存在，所以匿名可用）：

```json
{
  "code": -101,
  "message": "账号未登录",
  "data": {
    "isLogin": false,
    "wbi_img": {
      "img_url": "https://i0.hdslb.com/bfs/wbi/7cd0849…b84077c.png",
      "sub_url": "https://i0.hdslb.com/bfs/wbi/4932caf…70ac45.png"
    }
  }
}
```

这两个值**不是密钥**：由匿名接口下发、全站共用、每日轮换，公开可见且随时会变，本文因此只写出首尾片段。它们之所以要被记下来，是因为 WBI 签名由它们推出，而签名一旦算错，从外面只看得到 API 回一个 `code:-352`。**完整的一组实测向量固化在 `tests/bili_test.cpp` 里**，那是它该待的地方；文档不重复，以免两处各写一份、日后只改一处。

`room_init`（`/room/v1/Room/room_init?id=`）**完全不需要签名或登录**，并且它是把短房间号换成真实房间号的唯一途径。

## 一个自己踩到的传输层坑：SSL_read 必须先于 poll

非阻塞 socket 上，**先 poll 再 `SSL_read` 一次是错的**。当一条 TLS record 跨 TCP 段到达时，`SSL_read` 会把缓冲区里已有的字节消费掉、返回 `SSL_ERROR_WANT_READ`，而调用方把这当成超时。

症状极具迷惑性：TCP 连上了、TLS 握手成功、请求也写出去了，然后卡在 `upgrade response timed out`——尽管服务器已经回了 `101 Switching Protocols`。

用打印 `poll`/`SSL_read` 返回值的办法定位到的：

```text
poll(events=1,to=10000)=1 revents=1   ← 数据可读
SSL_read=-1                            ← 但返回 WANT_READ
```

对照实验证明与 B 站无关：同一份请求字节，用**阻塞** socket 的裸程序可以正常拿到 101。所以正确写法是 `readSome()` 现在的样子——**先 `SSL_read`，只在 OpenSSL 表示还要更多字节时才 poll**，且整段操作用一个 deadline 而不是每次重置。`writeAll()` 同理。

这个坑在 M0 之前不出现，是因为那时的测试都是阻塞 socket。

## 已实测观察到的弹幕字段

`DANMU_MSG` 的 `info` 是**一个**数组（不是多个 `info` 键），元素含义：

| 下标 | 含义 | 实测值 |
| --- | --- | --- |
| `info[0]` | 模式数组：`[?, mode, fontSize, color, timestampMs, rnd, ?, uidHash, ...]`，第 15 项是 `{"extra": "<json 字符串>"}` | `mode=1, fontSize=25, color=16777215` |
| `info[1]` | 弹幕正文 | `老丈人只为名` |
| `info[2]` | `[0, "昵称", ...]` | `旱獭邮箱_` |
| `info[9]` | `{"user": {...}, "extra": "<json 字符串>"}` | 含 `mode` / `dm_type` |

**表情弹幕的正文里是字面量 token**，不是图片也不是编号：

```text
[夏日热浪_想要]   [夏日热浪_爱你]   [夏日热浪_害羞]
```

格式为 `[表情名_类型]`。M1 画表情时按这个 token 切分正文。

`info[0][0]` 与 `info[9].extra` 里的 `mode` 在本次样本中恒为 `0`，与 `info[0][1]=1` 并存。**顶部 / 底部 / 逆向 / 高级弹幕的取值语义尚未实测**，需要在一个热闹房间里抓全模式样本再定；在此之前 `Danmaku::mode` 原样透传，不做映射。

### 匿名连接看不到昵称

```text
LOG_IN_NOTICE: {"notice_msg":"为保护用户隐私，未登录无法查看他人昵称"}
info[2][1] 实际值：旱獭邮箱_ / 蔷薇少女3 / 欠***
```

弹幕正文、颜色、字号、时间戳**全部正常**。所以弹幕显示本身不需要登录；要显示昵称才需要 `SESSDATA`。`--cookie` 就是为这个留的口子。

### 其他事件类型

实测在同一连接上出现过（`main.cpp` 只取 `DANMU_MSG`，其余记录在卡片上的 `last event`）：

```text
LOG_IN_NOTICE   NOTICE_MSG   WATCHED_CHANGE   ONLINE_RANK_COUNT   STOP_LIVE_ROOM_LIST
```

超级留言 / 上舰 / 送礼走同一条连接，只是 `cmd` 不同——M1 的付费卡片与上舰卡片不需要新连接。

## 视频节点侧：与视频节点库一致

节点注册、`SPA_PARAM_Buffers`、`TRIGGER` 不叠 `DRIVER`、`media.role=Production` 全部由 `lib/pw-video-simple-interface` 负责，本文不重复；那四条硬性要求与定位手段见
[`lib/pw-video-simple-interface/docs/internals.md`](../lib/pw-video-simple-interface/docs/internals.md)。

**一条要单独强调的坑**：`CairoFrame::blitTo()` 是**裁剪，不是缩放**（内部是 `min(w, width_)`）。协商尺寸小于配置尺寸时，右侧和下方直接被切掉，不会等比缩小。所以 **OBS 源的宽高必须等于 `--size`**，否则画面缺一块。

落盘验证：

```bash
./pw-live-danmaku --room 7734200 --seconds 45 &
sleep 6
gst-launch-1.0 -q pipewiresrc target-object=pw-live-danmaku num-buffers=10 ! \
    video/x-raw,format=BGRA ! filesink location=/tmp/frames.raw
ls -l /tmp/frames.raw        # 实测 17280000 = 10 × 480 × 900 × 4，逐字节吻合
```

没有消费者连接时流停在 `PAUSED`，渲染回调一次都不被调用——所以 OBS 里没开这个源时，弹幕侧开销为零。

## 为什么自己写 WebSocket 客户端

libcurl 8.x 其实带 WebSocket 支持（`curl_ws_recv` / `curl_ws_send` 符号存在），但没有采用，理由三条：

- libcurl 的 WebSocket 接口**官方标注为 experimental**；本项目只依赖发行版提供的稳定接口。
- 子模块的 `HttpClient` 是阻塞 GET 且不暴露 `CONNECT_ONLY`，用它就得为一个站点去改子模块。
- 剩下要自己写的只有 RFC 6455 的成帧，约 250 行，而 TLS 层的 OpenSSL 本来就要。握手所需的 SHA-1 与 base64 也出自 OpenSSL，没有引入任何新依赖。

## 凭据处理

`--cookie` 会把会话写进进程参数，任何本地用户都能从 `ps(1)` 读到，所以它会打印警告。`--cookie-file` 读文件，可 `chmod 600`，值不进命令行。两种方式下 cookie **都不会被写进日志**，也不会出现在任何错误信息里。

`--cookie` 与 `--cookie-file` 互斥；`--room` 缺失、两者同时给、cookie 文件读不到，都返回非零退出码（**绝不会是 0**，否则 systemd `Restart=always` 下会变成静默重启循环并报告 SUCCESS）。

## 未实测 / 已知缺口

- 顶部 / 底部 / 逆向 / 高级弹幕的 `mode` 取值语义（需要热闹房间的全模式样本）。
- 弹幕洪峰下的行为（本机实测的三个房间都很安静，45 s 内最多 3 条）。热门房间可能到每秒数十条，届时需要有界队列与丢弃策略。
- 断线重连：M0 失败即退出并把原因打在卡片上，**故意不做静默重连**——静默重连恰好会掩盖本里程碑要观察的东西。
- 头像与表情图（Twitch 侧才有真正的图片需求；B 站匿名连接没有头像）。

## 调试方法

不开 OBS 也能看到全部状态：`--dump` 直接出一张 PNG，卡片上写着当前处于哪一步。

```bash
./pw-live-danmaku --room 545068 --dump /tmp/x.png --count 3 --verbose   # 等到收到 3 条为止
```

单测（协议层无 UI，直接测）：

```bash
make test
```