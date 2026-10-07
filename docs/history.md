# 跨重启的历史

面板是**窗口**不是日志：留最近若干行，更早的忘掉。所以直播中途才打开 OBS，画面是从空列开始的
——房间明明正在聊。`--history N` 把同一个窗口同时留在盘上，下次启动先画出来，中间一条分割线。

## 用法

```bash
./pw-live-danmaku --room 545068 --history 60        # 记住最近 60 行
systemctl --user restart pw-live-danmaku             # 下次启动：旧消息在上，分割线，新消息在下
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--history N` | `0` | 持久化最近 N 行，下次启动先画出来，中间一条分割线。**`0` = 不持久化**，此时不写盘也不读盘 |
| `--history-file PATH` | `$XDG_STATE_HOME/pw-live-danmaku/history.json` | 历史文件位置；开头的 `~` 会展开为 `$HOME` |
| `--history-label TEXT` | `上次` | 分割线上的文字；传空字符串则只画线 |

删历史直接 `rm` 那个文件即可，程序读不到就当没有历史启动。

**醒目留言不入库**：它按金额算停留时间，把昨天的那条重新置顶是对付费时间撒谎。入场、礼物、点赞、
舰长卡片都是滚动列里的行，照常入库。

## 行为上的四点，都是踩过才知道的

- **写入是原子的**：先写同目录下的临时文件再 `rename`。写到一半掉电只会丢掉这一批，上一份还在；
  半截文件是解析不出任何东西的文件——症状和「没有历史」一模一样。
- **节流**在调用方：攒够 8 行或者 2 秒才落一次盘，退出前再落一次。落盘失败只警告一次并关掉本轮
  的历史，**不会**每条消息刷一行日志。
- **窗口跨重启是连续的**：启动时读回的历史会填进同一个窗口，所以只跑了 2 分钟的那一次不会把上
  一次留下的历史挤成 2 行。
- **历史按房间分**：文件里记着房间号，换房间读同一文件会被跳过并说明原因，不会把两个直播间的聊
  天混在一起。

## 实现细节

### 一、写在哪个线程

记在**站点线程**（`Shared::append`），不是帧回调里。理由是帧回调**没有消费者时根本不执行**：
挂在后台放了一下午，开播时有几十条消息本该留下，结果一条也没记。`append()` 只在锁内做一次
deque push，`HistoryStore::add()` 不碰磁盘，只回答「现在该落盘了吗」，真正的 `save()` 在锁外。

### 二、读回来的行要自己 `request()` 图片

程序里只有 `publish()` 会 `avatars.request()` / `emoteImages.request()`，而它只看实时流量。所以读回
历史的那段必须自己请求一遍，否则**每一行读回的头像都是灰色占位圆**——面板里最显眼的一行类型，
整列全灰。修法就是把 restore 挪到两个 `ImageStore` 构造之后，并在 `rebuildLayer()` 之前把 URL 递进去。

头像这样就够了：`drawAvatars()` 每帧重读缓存，晚到的图下一帧自己出现（见
[头像被烘焙进静态层，晚到的图永远补不上](internals.md#二头像被烘焙进静态层晚到的图永远补不上)）。
**表情与礼物图标不行**：它们由 `paintRow()` 烘焙进静态层、不重画，所以读回时图还没到就一直是 token
文字。这和实时行是同一个取舍——请求本身让缓存热的场合（同一个表情一晚上被全房间反复用）能直接命中，
真正的修法是让静态层自己重绘，比这个修复大。

### 三、落盘节流：8 行或 2 秒，谁先到算谁

按行数是为了洪峰时一次写盘而不是每条一次；按时间是为了安静的房间里那几条消息也能落盘，而不是一直
躺在内存里等一件更大的事情。**这一轮的第一条立刻写**——否则进程在第八条之前被杀，盘上什么都没有，
而历史恰好是那种「想要的时候才发现没有」的功能。退出前再写一次，落点在所有线程 join 之后。

`unsaved_` 只由 `save()` 清零，不由 `add()` 清零。这样即使调用方忘了看返回值，最坏也只是晚写，
**退出那次 `save()` 仍然兜底**；反过来（`add()` 里清零）就会静默丢掉这一批。

### 四、原子写：临时文件 + `rename`

写到一半掉电，剩下一份截断的文件——而**截断的 JSON 解析出来是空的**，症状和「没有历史」一模一样。
所以先写同目录下的 `.tmp` 再 `rename`，`rename` 在同一文件系统内是原子的，盘上要么是上一份完整文件，
要么是这一份。读的时候按 `format` 字段拒绝不认识的文件，避免未来版本的文件被半懂半猜地读成错的行。

### 五、种类与类型写名字，不写序号

序号在版本之间一动，存档里每一行就都变成另一种消息，而那种错**在 diff 里看不出来**，只在画面上看
出来（礼物画成了弹幕）。不认识的**名字**降级成普通弹幕，不认识的**序号**则无解。

### 六、醒目留言不入库

醒目留言在置顶图层里按金额算停留时间（1 分钟～2 小时）。把昨天的那条重新置顶，等于对着观众谎报它
是什么时候付的钱。

### 七、分割线是列里的一行，不是画面上的一层

分割线画在 `Panel::rebuildLayer(msgs, divider=true)` 里，**占一条行的高度**，跟着 `contentH_` 参与
滚动和裁剪。如果把它当成固定在画面底部的一层，新消息会从它**上面**长出来，视觉上就成了「实时弹幕在
上、历史在下」——正好把两者的关系说反了。作为一行，它随历史一起被顶上移，最后一起移出画面。

线宽用文字描边同样的 `1.5`，并且**先描一道更宽的深色再画浅色的本体**：这个 alpha 下的发丝线遇到亮画面
会直接消失，而亮画面恰恰是唯一要紧的背景。标签居中，线在标签两侧断开（`gapHalf` 取标签半宽加 10px，
并夹到线长的一半以内，否则窄面板 + 长 `--history-label` 会把两段线画反）。

### 八、systemd：StateDirectory

unit 里有 `ProtectHome=read-only`，`~/.local/state` 也在其中，所以光加 `--history` 会写不进去。
`StateDirectory=pw-live-danmaku` 建目录并只给这一个目录写权限。即便这条被删掉，程序也只是
「没有历史」而不会启动失败——**写盘失败只警告一次并关掉本轮历史**，不会每条消息刷一行日志。

### 九、自定义 `--history-file`：`ProtectHome=read-only` 一样会锁住

`StateDirectory=` 只放行它自己那一个目录。把 `--history-file` 指到 `$HOME` 下的别处——最容易踩
的就是和 cookie 做邻居，`%h/.config/pw-live-danmaku/history.json`——那个目录仍然是只读的，第一次
写盘就失败：

```text
[history] cannot write /home/.../.config/pw-live-danmaku/history.json.tmp: Read-only file system
[history] history is off for the rest of this run
```

症状是启动横幅永远 `0 restored`；房间没开播时连上面那两行都要等到下一条弹幕才出现，所以只看横幅
很容易以为历史功能没实现。修法是给那个目录单独开权限：

```ini
ReadWritePaths=-%h/.config/pw-live-danmaku
```

一个 systemd 的坑：**同一个路径不要同时出现在 `ReadOnlyPaths=` 和 `ReadWritePaths=` 里**——完全
相同的路径会让只读那一边生效，`ReadWritePaths=` 被顶掉，写盘照旧失败。unit 模板里那行
`ReadOnlyPaths=-%h/.config/pw-live-danmaku` 是给 cookie 的，要写这个目录就得去掉它、或收窄成
`-%h/.config/pw-live-danmaku/cookie`。收窄只解决「互相顶掉」，不保证 cookie 还是只读：父目录一旦
可写，subpath 的只读在实测里拦不住——好在那个目录本来就只有这个服务在写。

改完 unit 要 `systemctl --user daemon-reload` 再 `systemctl --user restart pw-live-danmaku`，
`systemd-analyze --user verify <unit>` 可以先验证语法。

## 端到端复现

手写一份历史文件，让程序读回来并与真实弹幕叠在一起：

```bash
printf '%s' '{"format":1,"saved":"2026-10-04T12:00:00Z","room":"545068","messages":[
  {"kind":"text","type":"normal","user":"昨天的人","color":0,"avatar":"","ts":1791000000000,
   "amount":"","amountValue":0,"count":0,"mergeKey":"",
   "parts":[{"emote":0,"text":"这是上一次启动时留下的弹幕","url":"","px":0,"verb":false}]}]}' \
  > /tmp/h.json
./pw-live-danmaku --room 545068 --history 20 --history-file /tmp/h.json \
                  --dump /tmp/e2e.png --count 3
# [history] restored 1 row(s) from /tmp/h.json
```

实测输出（房间 545068，2026-10-04）：读回 3 行手写的历史、画出分割线，再叠上本次真实连上来的 3 行；
`/tmp/h.json` 落盘后是 6 行、**没有残留 `.tmp`**。

格式与窗口的往返由 `tests/history_test.cpp` 覆盖（`make test`）。