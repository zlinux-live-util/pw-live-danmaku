# 实现细节

修改代码前先读本文。以下每条都来自实机验证，而非文档转述；其中若干条官方文档没有记载，或社区文档描述与实际行为不符。

本文与项目代码均在 LLM 辅助下编写。所有结论都经过实机复现——未经复现的推测不收录，因此每项都附有可重跑的对照数据或命令。

## 进度

**M0（协议打通）与 M1（聊天面板本体）已完成。**

链路：`room_init` → WBI 签名 `getDanmuInfo` → TLS WebSocket 握手 → JSON 认证包 → brotli 解包 → `Message` → cairo 渲染 → PipeWire 视频节点 → 消费者落盘。

M1 起画面就是聊天面板本身（不是调试卡片）。`--demo` 可以不连网调版面。

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

**但头像不一样：匿名连接照样拿得到。** 这一条纠正了本文早期版本写下的「B 站匿名连接没有头像」——那个结论是错的，因为它是从「昵称被掩码」推出来的，而不是实测。实测 `info[0][15].user.base.face` 是一个完整的 `https://i1.hdslb.com/bfs/face/....jpg`，无需任何凭据。所以头像走的是「拿到 URL → 后台线程下载」的普通图片流程，和 Twitch 侧需求一致。

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

## `DANMU_MSG` 的真实结构（M1 实测）

抓一条真实弹幕，`info` 有 **18** 个元素，实测输出：

```text
info 长度: 18
info[1] text   = 委屈吗？
info[2][1] name = 友好的益生菌
info[9]        = {}                      ← 是对象，且为空
info[0][15] is an object
  user present = yes
  base.face       = https://i1.hdslb.com/bfs/face/e9e57ba0...1b1cabe27.jpg
  base.name       = 友好的益生菌
  base.name_color = 0
  guard           = null
  medal.guard_lvl = 0   medal.name=德云色
  extra len=984
  extra.mode=0 dm_type=0 content=委屈吗？
  info[0][1..4] = mode=1 fs=25 color=16777215 ts=1791051148579
```

**关键：`extra` 和 `user` 都在 `info[0][15]`，不在 `info[9]`。** `info[9]` 是 `{ct, ts}`。

M0 版本的 `parseDanmaku` 写的是 `json["info"].at(9)["extra"]`，也就是说**它从来没读到过任何 extra**——`dm_type` 恒为 0 只是因为没人看。这个错误当时没能暴露，是因为 `mode` 恰好可以从 `info[0][1]` 拿到，输出看着是对的。**这是"字段读不到"与"字段读对"在外观上无法区分的一个例子**，只能靠打印真实结构来发现。

由此可得的面板需要的东西，全部匿名可得：

| 字段 | 路径 | 用途 |
| --- | --- | --- |
| 头像 | `info[0][15].user.base.face` | 24px 圆形头像 |
| 昵称 | `info[0][15].user.base.name` | 与 `info[2][1]` 一致 |
| 昵称颜色 | `info[0][15].user.base.name_color` | 十进制 RGB，0 表示无特殊色 |
| 上舰 | `info[0][15].user.medal.guard_level` 或 `user.guard.guard_level` | 1 总督 / 2 提督 / 3 舰长 |
| 表情表 | `info[0][15].extra.emots` | `{ "[token]": {url,width,height} }` |

**所以表情图片也是现成的**，不需要自己查表：`extra.emots` 直接给出 token → 图片 URL。

仍然拿不到的：**房管 / 主播身份**。这两个身份不在 `DANMU_MSG` 的载荷里，匿名连接无法区分，所以 B 站侧的用户名着色实际只有「普通」和「舰长」两档。这两个槽位留给 Twitch（`badges` tag 有 moderator/subscriber/broadcaster）。

## 表情弹幕的正文是字面量 token

`extra.emots` 的键就是正文里出现的那段字符串：

```text
[夏日热浪_想要]   [夏日热浪_爱你]   [夏日热浪_害羞]
```

所以切分规则是：**只有平台自己声明过的方括号串才算表情**，其余方括号按普通文本处理。`Bili::splitFragments()` 就是这么做的，单测里有一条专门钉住「未声明的方括号不被吞掉」。

## M1 渲染侧：三个必须遵守的约束

### 一、`TextRenderer` 只有一个 PangoLayout

`TextRenderer::layout()` 复用同一个 `PangoLayout`，每次调用都把它重新绑定到传入的 context。这在「一次排版、一次绘制」时是优点（比每次新建 layout 便宜），但**在遍历排版结果的循环里是陷阱**：

```cpp
PangoLayout* bl = text_.layout(cr, body, bs);          // 拿到整个正文的 layout
for (int i = 0; i < pango_layout_get_line_count(bl); ++i) {
  PangoLayoutLine* pl = pango_layout_get_line_readonly(bl, i);
  std::string slice = body.substr(pl->start_index, pl->length);
  text_.layout(cr, slice, ls);                          // ← 把 bl 本身覆盖掉了
  outlined(bl, ...);                                    // bl 已经指向 slice 的单行 layout
}
```

症状是**换行的第二行及以后整段丢失**，而且第一行的内容也会错乱。正确写法是**先把所有行区间收集到一个 vector，再开始画**。

### 二、测量与绘制必须用同一个折行宽度

第一版 `measureRow()` 用整个可用宽度排版，`paintRow()` 却用 `可用宽度 - 昵称宽度` 排版（因为首行要和昵称并排）。于是 measure 算出一行的高度、paint 画出两行，超出部分被下一条消息的移位覆盖——**又是一个「少了一截但看不出原因」的症状**。

现在 `measureRow()` 也先量昵称宽度，再用同一个 `avail - nw` 排版正文，两者必然一致。

### 三、入场动画会让「同一时刻的 update + render」拍到全透明

新消息不进静态层，而是作为「正在淡入的那一条」由 `render()` 逐帧叠加，200ms 后才烘进静态层。所以如果 `update(fresh, now)` 和 `render(cr, now)` 用**同一个** `now`，动画进度就是 `t=0` → alpha 0 → **整帧空白**。

这不是理论问题：M1 的 `--dump` 第一版就是这样，对着一个确实收到 3 条消息的房间拍出了一张全空白 PNG。

`--dump` 因此先连续渲染 12 帧（每帧 +33ms）再取最后一帧，把动画推到结束之后——与姊妹项目对自己有状态后处理效果的做法一致。

## M1 的性能做法

**静态层 + 增量移位。** 文字排好之后就不再变，所以整个画面累积在一张 ARGB32 静态层表面上；每来一条新消息，只把已有内容整体上移一行的高度，再在新露出的底部条带里画这一条。

- 「上移一行」是把表面画到自身偏移一行的位置——这是**自重叠 blit**，pixman 会正确处理重叠区，行为等同 `memmove`，不会糊。
- 因此**每条消息的开销正比于它自己的高度，而不是整个面板**。M0 的做法（每帧重新排版全部可见行）会让开销正比于可见行数 × 消息速率，在热闹房间里成为主要开销。
- 帧路径上没有新消息时，一帧就是一次整块拷贝。
- `rows_` 里已经滚出面板的行会被裁掉，因此不会随运行时长增长；面板也不保留已绘制的消息正文（像素已经在静态层里），所以帧回调不需要为它们分配任何东西。

**实测**（默认尺寸 **480×1080**，30fps，消费者接满）：

```text
window        : 30.0 s
frames pushed : 900  -> 30.0 fps
cpu           : 0.900 s  -> 3.00% of one core
per frame     : 1.000 ms
private memory: 94 MB
```

数字里包含：静态层整块拷贝（480×1080×4 ≈ 2.1 MB/帧）、每帧重画的 36 个 24px 头像圆盘、以及 b站协议解码。消费者断开时帧回调一次不被调用，渲染开销为零。

早期版本在 480×900 下曾量到「0 tick」，那是**采样错误**（读到了错误的进程或时刻），不是真实开销；上面这个用精确窗口 + 帧数差值重测过。

注意「0 tick」这个量级下测量方法本身是粗的（100 tick = 1s），只能说**远低于 3% 单核**，不能当作精确值。要更细的数需要 `perf` 或更长的采样。

## 卡片类消息：字段来自文档，尚未在本机观测到

`SUPER_CHAT_MESSAGE` 与 `GUARD_BUY` 走的是同一条连接（只是 `cmd` 不同），字段名取自 `pskdje/bilibili-API-collect` 的 `docs/live/message_stream.md`：

| cmd | 取用字段 | 渲染 |
| --- | --- | --- |
| `SUPER_CHAT_MESSAGE` | `data.uname` / `face` / `message` / `price` / `start_time` | 付费卡（青色） |
| `GUARD_BUY` | `data.username` / `guard_level` / `price`（金瓜子，1000 = 1 元） | 上舰卡（实心绿） |

**这两条尚未在本机抓到真实样本**，所以解析器里每个字段都是可选的：字段对不上时得到一条内容更少的消息，而不是把消息丢掉。面板侧的两种卡片样式则已经按参考 CSS 实现在 `--demo` 里（并有单测钉住字段映射）。

## 头像：四个独立的 bug，症状都是「看起来不对」

这一节值得单列，因为同一条路径上前后踩了四个坑，**每一个的症状都是「画面不对」，而每一个的真正原因都不在它看上去的地方**。而且前两个只有实机才能暴露。

### 一、缓存容量和解码尺寸用了同一个字段

`ImageStore` 最初只有一个 `size` 字段，同时是 LRU 容量**和**解码像素尺寸。传 `96` 做容量，于是每张头像按 **96×96** 解码，而面板画进的是 24px 的框：

```cpp
cairo_set_source_surface(cr, surf.get(), cx - r, cy - r);  // 96x96 图锚在左上角
cairo_paint(cr);                                           // 被 24px 圆裁掉
```

cairo 把源图像锚定在**左上角**，所以 24px 的圆裁到的正是那张图的**左上角 24×24**。现象是「只显示头像的一小块」。

教训：一个字段承担两种职责时，读代码看不出问题，只有数字对不上才会暴露。容量与像素尺寸已拆开（48px 解码 / 256 缓存），并且 `drawAvatar` 不再假设尺寸已正确——先缩放再画，将来万一再不匹配也只是缩放略有损失。

### 二、头像被烘焙进静态层，晚到的图永远补不上

面板把每行**烘焙**进静态层后不再重绘，而头像当时是跟着一起烘焙的。所以头像在消息到达那一刻还没下载完时，烘进去的就是灰色占位圆；**图后来真的到了，也永远没人重画那一行**。

这个 bug 的迷惑性在于**所有计数都正常**：`fetched=48 failed=0`、空 `face` 字段 0 个。图确实到了，只是到的时候没有人重画。

现在头像不烘焙，每帧从行的几何位置画（几十个 24px 小圆，开销可忽略）。迟到的图**下一帧自然就在**，不需要任何重绘机制。

### 三、动画行被画了两次

改成每帧画之后，`update()` 仍然把动画中的那行 push 进 `rows_`，于是 `drawAvatars()` 画了一遍（无位移的最终位置），动画 group 里又画了一遍。表现是**头像钉在原地不动，而旁边的文字从左边滑过来**。

修法：`animating_` 时 `drawAvatars()` 跳过 `rows_.back()`，那行交给 group 负责，头像和文字一起位移、一起淡入。

### 四、位置公式用了下边缘而不是上边缘

```cpp
const double bottom = r.y + r.h;
const double screenTop = height_ - (contentH_ - bottom);   // ← 多了 r.h
```

可见窗口是内容的最后 `height_` 像素，所以一行的**上边缘**映射到屏幕要减去窗口起点：`screenTop = height_ - (contentH_ - r.y)`。用下边缘等于多加一个行高，**每个头像都比自己的文字低整整一行**。

而动画行走 `render()` 里的 `height_ - animRowH_`，那个是对的——于是「滑入的头像」和「静止的头像」各自都对不上自己的文字，看上去就像滑入动画的头像对不上实际头像。

**这个 bug 静态 dump 里一眼可见**，前三轮没看出来是白捡的：它与前三个不同，不需要实机复现。

### 两种「灰」不是一回事

- **站方默认头像**：URL 是 `https://i0.hdslb.com/bfs/face/member/noface.jpg`，文件名就写着 noface。多个不同用户共用同一个文件。浅灰色「电视头」火柴人。
- **占位圆盘**：本项目在图未到达时画的纯灰圆。

实测一个热门房间里两者同时存在（48 个不同头像里，51 次 URL 观测中有 4 次重复，全部指向 noface.jpg）。**分不出来的时候要看计数，而不是看画面**：`failed=0` 且 URL 有重复 ⇒ 站方默认；`failed` 不为 0 或 pending 不降 ⇒ 占位。

## 表情弹幕的图片

`extra.emots` 直接给出 `{ "[token]": {url,width,height} }`，所以图片不需要自己查表。

排版做法：**先把每个表情替换成一个对象替换字符 U+FFFC，交给 pango 排版**，再把每个视觉行在 U+FFFC 处切开，交替绘制文本段和图片。这样**换行由 pango 决定**，表情落点由 pango 的排版结果决定；如果自己按 fragment 手工断行，就得重新实现一遍中英文混排的断行点，那才是难的部分。

U+FFFC 本身不会被画出来。图片没到时把 token 按字面文本画在图片应在的位置，消息仍然可读。

表情与头像**用两个 `ImageStore` 实例**：表情是一个被所有人反复复用的很小的集合，值得比头像大得多的缓存；头像则一人一张。

## 未实测 / 已知缺口

- `SUPER_CHAT_MESSAGE` / `GUARD_BUY` 的真实载荷（本机这几个房间都太安静）。
- 顶部 / 底部 / 逆向 / 高级弹幕的 `mode` 取值语义。对叠加层来说**这很可能不重要**：面板本来就把它们当普通文本行渲染，`mode` 只影响弹幕在视频上的位置，不影响聊天气泡。
- 弹幕洪峰。本机样本房间 30 秒最多 29 条；热门房间会到每秒数十条。已有界队列（`kPendingCap=400`）与每帧上限（`kMaxPerFrame=40`）兜底，但丢弃策略尚未在高流量下验证。
- 表情**图片**已经接上（`extra.emots` 的 URL + 独立的 `ImageStore` + U+FFFC 内联排版），但**只在实机验证过**：本机房间的表情频率不够高，`--dump` 一次不一定能抓到带表情的消息。
- Twitch：IRC 正在退役（见评估），EventSub 需要用户自带 token。

## 调试方法

不开 OBS 也能看到全部状态：`--dump` 直接出一张 PNG，卡片上写着当前处于哪一步。

```bash
./pw-live-danmaku --room 545068 --dump /tmp/x.png --count 3 --verbose   # 等到收到 3 条为止
```

单测（协议层无 UI，直接测）：

```bash
make test
```