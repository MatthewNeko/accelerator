# Accelerator + SourcePawn 插件崩溃归因（成品包）

上游 Accelerator 只能告诉你"哪个 C++ 模块炸了"。这个分支在 minidump 写完之后，
再多走一步：把 SourcePawn 虚拟机的调用栈打出来，落到崩溃报告的旁边那份 `.txt`
里。于是同一份崩溃，你能直接看到是哪个插件、哪一行、通过哪个 native 把进程带进
沟里的。

虚拟机栈只存在于进程临死的那一刻，dump 里读不到（JIT 代码不属于任何模块，
插件的调用帧也不是机器帧），所以这件事必须在崩溃回调里做。设计取舍和验证范围在
`ENGINEERING-NOTES.md`（英文）。

## 包里有什么

```
linux-x86/addons/sourcemod/          Windows 服请用 windows-x86/，两边只装一套
  extensions/accelerator.ext.so        扩展本体（32 位）
  extensions/accelerator.autoload      让 SM 开图自动加载
  gamedata/accelerator.games.txt
  scripting/include/accelerator.inc    多出 Accelerator_GetPluginStack 一个 native
  scripting/accelerator_stack_probe.sp 探针源码，想自己重编就用它
  plugins/accelerator_stack_probe.smx  自检与可控崩溃插件
install-linux.sh / install-win.ps1    带备份与校验的安装脚本
viewer.html                            离线看报告的网页，双击就开
checksums.txt                          包内每个文件的 SHA-256
```

适用面：CS:GO Legacy（32 位 x86）专用服，SourceMod 1.11（实测版本 1.11.0.6970）；
`sp_vm_api.h` 与 1.12-dev 一致，所以 1.12 的 ABI 也在同一代。**64 位服务器没有构建、
没有验证过**，别拿这个包去装。

## 装

先跑一遍不写文件的演练，看清会动哪些东西：

```
bash install-linux.sh /srv/srcds/csgo --dry-run
```

正式安装：

```
bash install-linux.sh /srv/srcds/csgo
```

Windows：

```
powershell -ExecutionPolicy Bypass -File .\install-win.ps1 -Server C:\steam\csgo
```

脚本会做三件事：把同名旧文件移进
`addons/sourcemod/rollback-accelerator-spstack/<时间戳>/`（不是覆盖掉），按平台拷贝
五个文件，然后逐个跟 `checksums.txt` 对 SHA-256，对不上就报错退出。

回滚就是把那个时间戳目录里的文件搬回去，再删掉新装的。

## 配

两个开关都读 `cfg/sourcemod/sourcemod.cfg` 的 `Core` 段，默认都是开：

```
"Core"
{
    "CrashPluginStack"   "1"   // 0 = 崩溃时不走插件栈
    "CrashReportArchive" "1"   // 0 = 上传后不留本地副本
}
```

上传地址仍然是原来的 `MinidumpUrl`（默认 `http://crash.limetech.org/submit`）。
`webternet.ext` 只有上传才需要；不装它也照样写 dump 和报告。

## 装完先自证，别等真崩

服务器控制台：

```
sm plugins list      // 要能看到 accelerator
sm_accel_stack       // 用新 native 走一遍和崩溃时同一套取栈逻辑
```

`sm_accel_stack` 的输出至少要有一帧 native 加一帧调用它的脚本帧。如果打出
`Status=no-plugin-contexts`，说明扩展根本没接触到虚拟机，这时去试崩溃没有意义。

如果是插件在加载阶段就挂了，看它给的原因：

- `Accelerator_GetPluginStack is unavailable: accelerator.ext is either not loaded ...`
  ——要么扩展真没加载（`sm exts list` 里看不到 Accelerator），要么加载的是不带
  插件栈能力的旧构建。探针只认这个 native 存不存在，不再看别的名字。
- 早期版本的探针曾用 `LibraryExists("accelerator.ext")` 判断，而扩展注册的库名是
  `accelerator`，于是明明装好了也会拒绝加载。这个错误已修，换包里的新
  `accelerator_stack_probe.smx` 即可。

想确认整条交付链路，用（这会**真的把服务器弄崩**，只在测试服上做）：

```
sm_accel_crash confirm
```

它故意通过一个 native 崩掉，下面挂着两层脚本调用。看
`addons/sourcemod/data/dumps/<dump>.dmp.txt` 里的这一段，以及上传之后
`data/dumps/archive/` 里留下的副本。

## 报告长什么样

一次真实崩溃打出来的东西（下面是实测原文，不是示意）：

```
-------- SOURCEPAWN STACK BEGIN --------
VM-Version=standalone VM host
VM-Thread=938 Crashed-Thread=938
Frames=5
[0] native   "AccelProbeCrash"  called-by=accel_probe.smx
[1] script   "InnerTwo"  accel_probe.sp:12  plugin=accel_probe.smx
[2] script   "InnerOne"  accel_probe.sp:17  plugin=accel_probe.smx
[3] script   "AccelProbeMain"  accel_probe.sp:22  plugin=accel_probe.smx
[4] internal (accel_probe.smx)
Running-Plugins=1
plugin=accel_probe.smx frames=3 functions="InnerTwo <- InnerOne <- AccelProbeMain"
Legend=native=engine/extension function, script=plugin code with file:line, internal=VM helper frame
-------- SOURCEPAWN STACK END --------
```

`script` 那几行的行号是**该函数里发起调用的那一行**，不是函数定义行。
`native` 行给出出事时正在执行的引擎/扩展函数——这正好是 minidump 单独给不了的
那半边：它只有那个 JIT 地址，不知道是谁跳进去的。

`文件:行` 里的文件路径是**编译时**交给 spcomp 的路径，不是你服务器上的路径，
所以看到 `D:\projects\xxx.sp` 这类路径是正常的（谁编的就指向谁的目录）。包里这支
探针是刻意在 `scripting/` 目录下用裸文件名编的，所以它自己只会打
`accelerator_stack_probe.sp`。

`Running-Plugins=` 是同一份栈按插件汇总：每个有脚本帧的插件一行，函数**自内向外**。
跨插件调用（A 调 B）两边都会出现——虚拟机整个过程只有一条调用链，插件不需要额外声明
什么。超过 24 个插件时会出现 `(+N more not listed)`。

空值和怪样子都写出来，不留白，免得"没量到"看着像"量了没问题"：

| 看到 | 意思 |
| --- | --- |
| `Frames=0` + `Status=vm-idle` | 当时确实没有插件在跑，这次崩溃没插件可怪 |
| `Status=no-plugin-contexts` | 扩展手上没有已加载的插件上下文可问虚拟机 |
| `Status=iterator-unavailable` | 虚拟机不肯给帧迭代器 |
| `Frames=?` | 帧数没数出来（配合上面两种状态，或 Windows 上走栈再次异常） |
| `Status=walk-faulted` | 只有 Windows 会有：读栈触发了第二次异常，盘上的 dump 仍然有效 |
| `note=plugin compiled without debug info` | 那个 `.smx` 里没有 `.dbg.lines`，`:0` 是应该的，不是查丢了 |
| 整段没有 | Linux 上通常是走栈时二次异常/卡住被 watchdog 杀掉（前面几段仍在）；先确认 `CrashPluginStack` 不是 0 |

`VM-Thread` 和 `Crashed-Thread` 不一样时会多一行 `Caveat=`，提醒你下面这串是虚拟机
自己的栈，未必是崩掉那个线程正在干的事。

## 看报告：viewer.html

双击打开（纯本地页面，不联网、不上传、不写文件），把整份 `<dump>.dmp.txt` 贴进去，
或者只贴 `SOURCEPAWN STACK` 那一段，按「解析」。它会：

- 把结论写在最前面的牌子上：有没有这段、段是否写完、是虚拟机空跑还是没有插件上下文、
  有没有截断；
- 按帧列出类型（native / script / internal）、函数、`文件:行`、所属插件；
- 统计脚本帧里各插件出现的次数，作为"先查谁"的提示——这是提示，不是结论，外层帧常常
  只是被无辜牵连的调用者；
- 对不上账时会明说：`Frames=3` 却只列出 2 帧，而段又自称完整、也没说被截断，那多半是
  贴漏了或者被改过。

页面里的「载入实测样例」装的是本机真实崩溃打出来的那份，用来确认工具本身在工作。
它的解析器覆盖上面表格里的每一种形状（包括 `(unknown file)`、`(unregistered plugin)`
这种带空格的占位名），并且每条样例都和 `extension/sp_stack.cpp` 里的格式串对过账，
C++ 改了写法而页面没跟上时，`node tests/check_viewer.mjs` 会红。

## 控制台记录

报告里 `CONSOLE HISTORY` 那一段只保留**最后 500 行**（现场在末尾），段首写
`Source=`、`Lines=保留/持有`，真截过时会多一个 `(tail kept)`——所以"少了行"和
"本来就这么点"能分清。

现在的来源按顺序是两个：

- **扩展自己的日志监听器**（`Source=log-listener`）：加载时向 tier0 注册
  `LoggingSystem_RegisterLoggingListener`，把控制台输出留在一个 128 KB 的静态环里。
  这和 SourceMod 的 `sm_conhook` 用的是同一个入口，按时间顺序；卸载扩展时会注销
  （引擎没导出注销入口的版本干脆不注册，避免留下指向已卸载代码的指针）。
  **它只从扩展加载那一刻开始记**，加载之前的输出不在里面。
- **引擎的 `GetSpew`**（`Source=engine-getspew`）：监听器没内容时才用它；在
  CS:GO Legacy 1.38.8.1 的 Linux 引擎上它永远返回空——`csgo` 段的字节签名仍然能
  定位到函数（engine.so+0x2dcbe0），但它遍历的链表在构造之后就再没人写入过，
  引擎自己那句 `Console History (reversed)` 也一个引用都没有了。Windows 上它仍然
  可用，而且能补上扩展加载之前的那段。

| 看到 | 意思 |
| --- | --- |
| `Source=log-listener` | 行来自扩展自己的环；`Lines=保留/持有` 的"持有"是环里当前行数，不是开机以来总数 |
| `Source=engine-getspew` | 监听器为空，用了引擎历史；`Lines=保留/总数` 的"总数"是引擎缓冲 |
| `Status=console-empty` | 两个来源都查了，都没东西 |
| `Status=console-unavailable` | 两个来源都不可用；`Note=GetSpew:` 和 `Note=log listener:` 各说明原因，启动日志里有同样的 WARNING |

如果你怀疑某个版本的引擎有没有这条历史缓冲，可以看二进制里还有没有那句字符串：
`grep -rl "Console History" bin/`。有字符串不代表有代码在写它——1.38.8.1 就是
只剩字符串的情况，真正的判据是日志监听器有没有收到行。

## 还没在你服务器上验过的

包里的二进制和取栈逻辑是在真虚拟机 + 真 `SIGSEGV` 上实测过的。控制台监听器也在
本机 Linux 专用服上单独验过：把一个只注册监听器的 MetaMod 插件挂上去，服务器跑
35 秒后环里已有 404 行真实的启动输出（`installed=1`、`held=404`），说明 tier0 的
日志确实会送到这个入口——顺带抓到 `dlsym(RTLD_DEFAULT)` 在真服上取不到 tier0 符号，
已经改成先找到已加载的 `libtier0.so` 再取。

还差的两件事只有起了真服才能收口：崩溃回调 → `.dmp.txt` → 上传/留档这条链路
（包括带控制台段的那份报告），以及 Windows 那条 `__try/__except` 保护路径（目前
只过了编译）。原因写在 `ENGINEERING-NOTES.md` 最后两节。
