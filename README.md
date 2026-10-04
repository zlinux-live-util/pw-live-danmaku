# pw-live-danmaku

**把 B 站直播间的弹幕，做成一个透明叠加层，以 PipeWire 视频节点输出给 OBS。**

单进程 · 无浏览器 · **零子进程** · 没有消费者连接时不渲染

---

## 这是什么

一个独立的 PipeWire 视频节点，内容是一块**直播聊天面板**：头像、彩色昵称、正文、内联表情、
付费留言与上舰卡片，背景透明，可以直接叠在直播画面上。

它替代的是「用浏览器源 + 一大段 CSS 定制 YouTube 聊天 DOM」那套做法。本项目只依赖发行版
自带的系统库，没有浏览器、没有 Chromium/OBS 浏览器源，也不需要任何包管理器。

> 当前状态：B 站可用；Twitch 尚未实现（IRC 正在退役，EventSub 需要用户自带 token，见
> [文档](#未做的事)）。

## 特性

- **聊天面板**，不是滚动弹幕：底部对齐、角色配色、左侧色条、折行缩进
- **头像**实时下载并裁成圆形；**表情弹幕**按 B 站给的内联图片渲染
- **礼物 / 入场 / 点赞**都画进同一列：礼物带图标和数量，入场与点赞压暗、不抢弹幕的注意力
- **醒目留言**独立置顶：顶边对齐、自动换行不省略、按金额停留 1 分钟～2 小时后淡出，可同时挂多条
- **上舰卡片**：实心绿卡片，跟滚动列表一起走
- **入场动画**：新消息立刻占位、把旧消息顶上去，再滑入
- **完全透明**，只有文字自带描边，叠在明亮画面上也读得清
- **按字面渲染任何文本**——弹幕里出现的 `<script>` 就是字形，不做任何标记解析
- 尺寸任意（`WxH`）、帧率上限可调
- **不连网也能调版面**：`--demo` 渲染一组固定消息

## 构建

依赖（Arch）：

```bash
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire curl openssl \
                   brotli zlib
git submodule update --init --recursive   # 视频节点库是子模块
make                                      # -> ./pw-live-danmaku
make PORTABLE=1                           # 同上，但不加 -march=native（分发或换机器时用）
```

视频节点实现来自 [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)
（git 子模块），与姊妹项目 `pw-mpris-visualcard` 共用同一个 pin。

## 在 OBS 里使用

OBS 自带的 `linux-pipewire` 走 xdg-desktop-portal，只能捕获屏幕与窗口，**选不到本节点**；
需要配合插件 [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo)。

1. 来源 **+** → **PipeWire Video**
2. 下拉框里选 **Live Chat**
   - 下拉框**显示的是节点描述**（`--desc`），实际连接的是节点名（`--node`）——这是两个不同字段
   - 插件只在**打开属性对话框那一刻**枚举一次节点；列表里没有的话，确认进程在跑，然后关掉再打开对话框
3. **把源的宽高设成和 `--size` 完全一致**（默认 `480x1080`）
   - ⚠️ 协商尺寸比配置小的时候画面是**被裁掉**，不是等比缩小。尺寸不一致就会缺一块
4. 叠加到场景上即可，背景本来就是透明的

## 用法

```bash
./pw-live-danmaku --room 545068
./pw-live-danmaku --room https://live.bilibili.com/545068    # 短链 b23.tv 也可以
./pw-live-danmaku --demo --dump /tmp/panel.png               # 不连网调版面
```

开机自启：

```bash
make install-service                             # 渲染 unit 到 ~/.config/systemd/user/
systemctl --user enable --now pw-live-danmaku
systemctl --user restart pw-live-danmaku         # 改过参数后重启才生效
```

## 昵称默认是掩码的

未登录连接拿得到弹幕正文、颜色、头像和表情，但 B 站会对**昵称**打码（`为保护用户隐私`）。
要真昵称，从浏览器 devtools 里拷一份 cookie：

```bash
printf '%s' 'SESSDATA=...; bili_jct=...; DedeUserID=...' > ~/.config/pw-live-danmaku/cookie
chmod 600 ~/.config/pw-live-danmaku/cookie
./pw-live-danmaku --room 545068 --cookie-file ~/.config/pw-live-danmaku/cookie
```

`--cookie-file` 推荐而不是 `--cookie`：后者会把会话写进进程参数，任何本地用户都能从
`ps(1)` 读到。两种方式下 cookie 都不会被写进日志。

**打码不是全局的**：实测同一条匿名连接上，弹幕与入场的昵称被打码，而**点赞**与站方的入场动画
事件给的是实名。所以未登录时画面上仍然会有几个真名。理由与实测数字见
[docs/internals.md](docs/internals.md#这两个反直觉的实测结果)。

## 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--room ID\|URL` | 必填 | 房间号或直播间 URL；`b23.tv` 短链也能解析 |
| `--cookie STR` | | cookie 字符串。会进 `ps(1)`，程序会警告 |
| `--cookie-file PATH` | | 从文件读 cookie（推荐，可 `chmod 600`） |
| `--font NAME[,...]` | CJK 回退链 | pango 逐字符回退，拉丁与中文字体可以串成一条链 |
| `--font-size N` | `28` | 聊天区字号（用户名与正文）。头像框随之为一行文字高 |
| `--card-font-size N` | `30` | 醒目留言 / 舰长卡片内的字号 |
| `--font-file PATH` | | 启动时把字体文件注册进 fontconfig；可重复 |
| `--entry-merge MS` | `5000` | 入场事件合并窗口：这段时间内进来的人合成一行（`某某 等 7 人进入了直播间`）。实测热闹房间里入场是 1.57 条/秒、弹幕 0.36 条/秒，全量显示会把弹幕淹掉。**`0` = 不合并，每人一行** |
| `--node NAME` | `pw-live-danmaku` | PipeWire 节点名 |
| `--desc TEXT` | `Live Chat` | 节点描述，**OBS 下拉框里显示的就是它** |
| `--size WxH` | `480x1080` | 输出尺寸。**OBS 源尺寸必须与之一致** |
| `--fps N` | `30` | 帧率上限；消费者可协商更低 |
| `--demo` | | 渲染固定消息，不连网 |
| `--dump FILE` | | 出一张 PNG 后退出（会等头像与表情下载完） |
| `--count N` / `--seconds N` | `0` | 画了 N 行 / 跑 N 秒后退出。`--count` 数的是**进入面板的行**，所以被 `--entry-merge` 折叠掉的入场 notice 不计入 |
| `--verbose`, `-v` | | 协议握手、每条消息、头像与表情的下载计数 |

## 性能

默认 480×1080 @30fps、消费者接满、实测：

```text
window        : 30.0 s
frames pushed : 900  -> 30.0 fps
cpu           : 0.900 s  -> 3.00% of one core
per frame     : 1.000 ms
private memory: 94 MB
```

**没有消费者连接时，渲染回调一次都不会被调用**，面板侧开销为零。

做法是：文字排好后就不再变，所以整幅画面累积在一张静态层表面上，每来一条消息只把它上移一行
再在新露出的条带里画这一条——每条消息的开销正比于它自己的高度，而不是面板。头像和入场动画
那一条每帧重画，代价是几十个小圆盘。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/internals.md](docs/internals.md) | **改代码前先读**。协议实测结论、与社区文档相反的地方、踩过的坑与实测数据、调试命令 |

本文与代码均在 LLM 辅助下编写。所有结论都经过实机复现——未经复现的推测不收录。

## 未做的事

- **Twitch**：IRC 正在退役（社区标注 2026-09-02 deprecated），需要迁到 EventSub，而
  EventSub 读聊天要求用户自带带 `user:read:chat` 的 token。架构上已为它留好接口
  （`Site` 抽象、`Fragment` 里的表情片段），但实现还没写。
- **房间自动识别**：目前需要手填房间号。浏览器的媒体会话拿不到房间号（Firefox 的
  `xesam:url` 是 blob:），Chromium 系在 Linux 上没有 MPRIS。
- **断线重连**：目前失败即退出并把原因打在面板状态里。
- **热门房间的洪峰**：已有界队列（400）与每帧上限（40）兜底，但丢弃策略未在高流量下验证。
- `README.en.md`：暂无英文版。

## 许可

MIT。运行期使用的系统库均为动态链接，未捆绑、未修改其代码。视频节点实现取自
`pw-video-simple-interface`（同为 MIT）。OBS 侧的 `obs-pwvideo` 是 GPLv2 的独立程序，
与本项目之间只有 PipeWire 节点的运行时数据流，不构成链接或派生关系。