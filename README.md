# RemapCopilotKey

> **仓库地址：<https://github.com/Anlerway/RemapCopilotKey>** ｜ English: [README.en.md](README.en.md)

把新笔记本上被微软强制替换掉的 **Copilot 键**（等于 `左Win + 左Shift + F23`）改回真正可用的
**右 Ctrl**，或者映射成任意按键、组合键、打开程序、打开网页、Windows 实用功能。

配置文件是 JSON，界面支持多语言。

> ### ⚠️ 当前状态：开机自启**不可用**
>
> **请先读这一段，免得照着配一遍才发现没用。**
>
> 由计划任务拉起来的服务实例是**低完整性（0x1000）**，低级键盘钩子受 UIPI 限制
> **收不到任何按键**。现在唯一能用的是**直接启动**（双击 `RemapCopilotKeyService.exe`，
> 或在配置程序里点「启动服务」且**没有**注册自启），它的完整性是**高（0x3000）**，
> 工作正常 —— 代价是每次都要确认一次 UAC。
>
> 实测证据、四条失败路线和目前唯一可用的用法，见
> [开机自启：目前不可用](#开机自启目前不可用)。

```
┌──────────────────────────┐        config.json        ┌───────────────────────────┐
│  配置程序 (C# / WPF)      │ ───────────────────────▶ │  服务程序 (C++ / Win32)     │
│  · 勾选映射目标           │                          │  · 全局低级键盘钩子          │
│  · 多语言                 │ ◀── 2 秒轮询热加载 ────── │  · Copilot 三键序列识别      │
│  · 写 config.json         │       窗口消息控制        │  · 执行动作 / 后台静默       │
│  · 注册开机自启计划任务    │       计划任务启动        │  · 管理员权限运行            │
│  · 管理员权限运行         │                          │                           │
└──────────────────────────┘                          └───────────────────────────┘
```

两个进程**同级提权**运行（都是管理员），配置程序负责写配置和注册计划任务，
服务程序只负责拦截按键。

---

## 目录

- [为什么需要它](#为什么需要它)
- [功能特性](#功能特性)
- [工作原理](#工作原理)
- [目录结构](#目录结构)
- [构建](#构建)
- [安装与使用](#安装与使用)
- [配置文件格式](#配置文件格式)
- [开机自启：目前不可用](#开机自启目前不可用)
- [多语言](#多语言)
- [测试与验证](#测试与验证)
- [已知限制](#已知限制)
- [故障排查](#故障排查)
- [开发约定（踩过的坑）](#开发约定踩过的坑)
- [关于"未知发布者"](#关于未知发布者)
- [相关背景](#相关背景)

---

## 为什么需要它

微软在 [Windows Copilot Key Guidance for Ecosystem Partners](https://learn.microsoft.com/en-us/legal/windows/hardware/copilot-key-guidance)
里规定了 Copilot 键的行为：

> 按下时上报 `make LWIN` → `make LSHIFT` → `make F23`；
> 松开时上报 `break F23` → `break LSHIFT` → `break LWIN`。
> 长按时三个键会按 Win / Shift / F23 的顺序重复上报。
> 通常由负责键盘上报的**嵌入式控制器固件**实现。

而且当键盘上没有多余位置时，微软**要求**用 Copilot 键**替换掉右 Ctrl 键**。BIOS 和 Windows
设置里都没有把它改回来的开关。

这带来两个后果：

1. **任何"无条件替换扫描码"的方案都不成立**。注册表 `Scancode Map`、SharpKeys、
   PowerToys 的"重映射单个按键"都只能改 F23，改不掉固件一并发出的真实 LWin 和 LShift，
   结果会变成 `Win+Shift+Ctrl+…`。
2. **必须做序列识别**：只有确认这三个键按正确顺序、在很短时间内连续出现，才能认定是
   Copilot 键，否则就必须原样放行——不然会破坏正常的 Win 键和 Shift 键。

本项目做的就是第 2 件事。

## 功能特性

- **按键序列识别**：完整实现上面那套状态机，包括 100ms 时序窗口、按住重复上报、
  序列被打断时按原顺序补发被拦截的按键。
- **不干扰正常按键**：只用左 Win、左 Shift、F23 三键的**组合序列**触发；
  单独按 Win、单独按 Shift、`Win+E`、`Win+Shift+S` 等都按原样工作。
- **五种映射目标**
  - 映射为**按键**（按住 Copilot 时目标键保持按下，松开时一起松开——默认右 Ctrl）
  - 映射为**组合键**，**支持三连键**（如 `Ctrl + Shift + Esc`）
  - **打开应用程序**（可带启动参数）
  - **打开网页**
  - **Windows 实用功能**：资源管理器 / 任务管理器 / cmd / PowerShell / 截图 /
    截图工具 / 设置 / 控制面板 / 计算器 / 记事本 / 任务视图 / 剪贴板历史 /
    表情面板 / 锁定屏幕
- **JSON 配置 + 热加载**：配置文件放在程序目录，改完保存即刻生效，无需重启。
- **多语言界面**：内置简繁中英语言包，新增语言只需往 `Lang\` 丢一个 JSON。
- **后台静默**：服务程序没有托盘图标、没有窗口，出错只写日志。
- **输入捕获式快捷键设置**：配置界面里直接**按下键盘**来录制快捷键，不用翻下拉列表。
  （限制：Win 键被系统抢先处理，普通按键事件收不到；本项目的捕获控件另外临时挂了钩子，
  所以 Win 键也能录，代价是录制时开始菜单仍会弹出。）
- **零外部依赖**：C++ 侧自带 JSON 解析器，不引入任何第三方库。

> ❌ **开机自启目前不可用**：计划任务启动的实例是低完整性，钩子收不到按键。
> 详见[开机自启：目前不可用](#开机自启目前不可用)。

## 工作原理

### 服务程序（C++）

用 `SetWindowsHookEx(WH_KEYBOARD_LL)` 装一个全局低级键盘钩子，用状态机识别序列：

```
按下序列   Idle ──左Win──▶ LeftWindows ──左Shift──▶ LeftShift ──F23──▶ 【触发】
松开序列   Idle ──F23──▶ F23 ──左Shift──▶ LeftShift ──左Win──▶ Idle
```

- 序列识别过程中，遇到的 LWin / LShift **先拦截不发出**（suppress），
  等确认到底是不是 Copilot 键。
- 一旦凑齐三键 → 执行配置的动作，被拦截的 LWin / LShift **不再补发**，直接丢弃。
- 序列被打断（其它键按下 / 任何键松开 / 100ms 超时）→ 按 **LWin → LShift → 当前键**
  的正确顺序把被拦截的键补发出去。

关键实现细节：

- **不在钩子回调里调用 `SendInput`**。钩子回调有超时限制，而且注入的按键会再次进入
  自己的钩子。所以钩子只 `PostMessage` 到一个隐藏的消息窗口，由窗口过程完成注入。
- **注入的按键靠 `LLKHF_INJECTED` 标志放行**，避免自激。
- **注入时带完整扫描码与扩展位**，方向键 / 小键盘 / PrintScreen 等都做了修正，
  这样只认扫描码的游戏和 DirectInput 程序也能正常工作。
- **切到管理员窗口时强制释放按键**。低级键盘钩子受 UIPI 限制，非提升进程看不到送往
  提升窗口的输入；如果此时正按着注入的 Ctrl，就会卡住。程序监听前台窗口变化，
  离开管理员窗口时立刻补发松开。
- **识别 F23 时同时看虚拟键码和扫描码 0x6E**，即使用户用注册表把 F23 重映射过也能识别。

### 配置程序（C# / WPF，.NET Framework 4.8）

- 只负责**写 config.json**，不参与按键拦截。
- 保存后除了写文件，还会给服务程序发一条窗口消息让它立刻重载（文件监视本身有 2 秒延迟）。
- **注册 / 删除开机自启计划任务**：通过计划任务的 COM 接口在本进程内完成
  （见 `src/Configurator/AutoStart.cs`），不 shell 出去跑 `schtasks.exe`。
  关键在于 `RegisterTaskDefinition` 的 `TASK_LOGON_INTERACTIVE_TOKEN` 参数 ——
  它只存在于函数参数里，`schtasks` 表达不出来。原因见
  [开机自启是怎么做的](#开机自启是怎么做的)。
- 配置程序与服务程序**同级提权运行**（清单里是 `requireAdministrator`），
  所以注册计划任务不会被权限拒绝。

## 目录结构

```
RemapCopilotKey/
├─ RemapCopilotKey.sln             解决方案（三个工程）
├─ build.cmd / build.ps1            统一构建脚本
├─ package.cmd                      生产版打包的薄包装
├─ src/
│  ├─ Service/                      C++ 服务程序
│  │  ├─ app.manifest               requireAdministrator + DPI 感知
│  │  ├─ src/
│  │  │  ├─ main.cpp                入口、隐藏消息窗口、命令行
│  │  │  ├─ CopilotHook.{h,cpp}     键盘钩子 + 三键序列状态机
│  │  │  ├─ Actions.{h,cpp}         动作执行（注入按键 / 启动程序）
│  │  │  ├─ Config.{h,cpp}          配置模型、读写、热加载监视
│  │  │  ├─ Keys.{h,cpp}            虚拟键名 <-> 键码
│  │  │  ├─ AutoStart.{h,cpp}       权限与运行环境查询（会话 / 窗口站 / 桌面）
│  │  │  ├─ Log.{h,cpp}             日志（含不受 logLevel 影响的启动追踪）
│  │  │  └─ Json.{h,cpp}            自带 JSON 解析 / 序列化
│  │  └─ tests/
│  │     ├─ json_test.cpp           JSON 模块测试
│  │     └─ config_test.cpp         配置与键名测试（含跨语言契约）
│  ├─ Configurator/                 C# 配置程序（WPF / net48）
│  │  ├─ App.xaml(.cs)              应用入口（含 --dump-default-config / --export-task / --diagnose）
│  │  ├─ MainWindow.xaml(.cs)       界面
│  │  ├─ Assets/app.ico             程序图标（只给界面用，服务程序不设图标）
│  │  ├─ Themes/Styles.xaml         浅色 Win11 风格全局样式
│  │  ├─ HotkeyBox.cs               单键捕获控件（含 Win 键捕获）
│  │  ├─ KeyCatalog.cs              键位与实用功能目录
│  │  ├─ AppConfig.cs               配置模型（与 C++ 侧对应）
│  │  ├─ Json.cs                    自带的极简 JSON 读写（net48 无 System.Text.Json）
│  │  ├─ AutoStart.cs               开机自启计划任务注册（计划任务 COM 接口）
│  │  ├─ Localization.cs            多语言
│  │  ├─ ServiceControl.cs          与服务程序交互
│  │  ├─ Diagnostics.cs             --diagnose 启动日志（GUI 没有 stdout）
│  │  ├─ Compat.cs                  net48 上 C# 9 record/init 需要的 IsExternalInit
│  │  └─ Lang/{zh-CN,en-US}.json    语言包
│  └─ Configurator.Tests/           C# 侧配置契约测试（控制台）
├─ tools/
│  ├─ package.ps1                   生产版打包（绿色包 + 校验值 + 可选安装程序）
│  ├─ installer.iss                 Inno Setup 安装程序脚本
│  ├─ check-xml.ps1                 构建前：查非法 XML 注释
│  └─ check-manifest.ps1            构建后：查 exe 里嵌的提权清单
├─ docs/
│  ├─ 设计说明.md
│  ├─ 配置格式.md
│  └─ 开发笔记.md
└─ .github/workflows/               CI：构建 + 测试 + 打包
```

## 构建

### 环境要求

| 组件 | 版本 | 说明 |
|---|---|---|
| Visual Studio | 2019 以上（含 C++ 生成工具） | 提供 MSVC 编译器、MSBuild 和 .NET Framework 4.8 Targeting Pack |
| Windows SDK | 10.0.19041 以上 | |
| git | 任意 | |

> **不需要 .NET SDK，也不需要 NuGet**：配置程序是 .NET Framework 4.8 的 WPF 工程，
> 由 Visual Studio 自带的 MSBuild 编译，引用程序集取自自带的
> .NET Framework 4.8 Targeting Pack。构建脚本会通过 `vswhere` 自动定位 Visual Studio，
> **无需**手动打开"VS 开发者命令提示符"。

### 构建

**用 Visual Studio（推荐）**：直接打开仓库根目录的 `RemapCopilotKey.sln`，
选 `Release | x64` 生成即可。解决方案里包含三个工程：

| 工程 | 类型 | 产物 |
|---|---|---|
| `RemapCopilotKeyService` | C++ / MSBuild | `RemapCopilotKeyService.exe` |
| `RemapCopilotKey` | C# / WPF（net48） | `RemapCopilotKey.exe` |
| `ConfigContractTests` | C# 控制台测试 | 配置 JSON 契约测试 |

**用脚本**：

```bat
build.cmd
```

或者直接调用 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

### 构建子命令

```bat
build.cmd service     只构建 C++ 服务程序
build.cmd config      只构建 C# 配置程序
build.cmd test        只编译并运行全部单元测试（C# + C++）
build.cmd clean       删除 build\
```

构建前会先跑一次 XML 注释预检（`tools/check-xml.ps1`）：
`app.manifest` 里出现连续两个减号的注释会让清单变成非法 XML，
打出来的 exe 双击只报"应用程序的并行配置不正确"，而且没有任何线索。
MSBuild 与 XAML 编译器都不检查 manifest，所以这一步由脚本自己保证。

配置程序构建完还会跑一次清单检查（`tools/check-manifest.ps1`）：
确认 exe 里**真的**嵌进了要求管理员权限的清单。源码正确不代表产物正确 ——
这两步都是实际踩过的坑，详见 [docs/开发笔记.md](docs/开发笔记.md)。

产物（已被 `.gitignore` 排除）：

```
build\out\Service\RemapCopilotKeyService.exe      ← 服务程序
build\out\Configurator\RemapCopilotKey.exe        ← 配置程序
artifacts\release\                                ← package.cmd 的生产版产物
```

> **为什么用 PowerShell 而不是批处理？**
> `cmd.exe` 用 OEM 代码页解析 `.cmd`，遇到 LF 行尾会错乱，而且 `%ProgramFiles(x86)%`
> 在部分宿主环境里不存在会直接让脚本崩溃。`build.cmd` 只是一个三行的包装器。

### 发布打包（生产版）

```bat
package.cmd
```

流程：编译 → 组装发布目录 → 清除网络标记 → 压缩 → 生成 SHA256 →（可选）打安装程序。
产物在 `artifacts\`：

| 产物 | 说明 | 目标机器需要 |
|---|---|---|
| `release\RemapCopilotKey-v1.0.0-x64.zip` | **绿色包，解压即用**（约 0.23 MB） | 什么都不用装 |
| `release\RemapCopilotKey\` | 打包前的完整目录（已清除网络标记） | — |
| `release\SHA256SUMS.txt` | 所有产物的 SHA256 校验值 | — |
| `installer\RemapCopilotKey-v1.0.0-x64-setup.exe` | 安装程序（**可选**，约 2.2 MB） | — |

绿色包里只有 4 个文件：配置程序、它的 `.exe.config`、C++ 服务程序、`使用说明.txt`。
编译中间产物（`.pdb`）一概不打包。

参数：

```powershell
# 只用已有产物打包，不重新编译；跳过安装程序
powershell -NoProfile -File tools\package.ps1 -SkipBuild -NoInstaller

# 指定版本号（会写进产物文件名）
powershell -NoProfile -File tools\package.ps1 -Version 1.1.0
```

安装程序需要 [Inno Setup 6](https://jrsoftware.org/isinfo.php)（`ISCC.exe`）。
**没装就自动跳过，不算失败** —— 绿色包才是主要交付方式。

> **为什么只有一个绿色包，而且只有 0.23 MB？**
> 配置程序是 .NET Framework 4.8 的 WPF 应用，而 net48 是 Windows 10 1903+ /
> Windows 11 **系统自带**的组件，因此不需要自带运行时。

> **关于网络标记**：打包脚本会对本机产物执行 `Unblock-File`，
> 但它只对**本机产出的文件**有效（zip 内部的标记要在解压前解锁 zip 才有效）。
> 从网上下载的用户解压后仍可能看到"安全警告"，要根治只能做代码签名。
> 本项目**不做代码签名**（原因见[关于"未知发布者"](#关于未知发布者)）。

## 安装与使用

1. 构建，然后把 `build\out\Service\RemapCopilotKeyService.exe` 和
   `build\out\Configurator\` 里的全部文件放到**同一个目录**（例如
   `C:\Program Files\RemapCopilotKey\`）。

   > 配置程序读写**自己所在目录**的 `config.json`，服务程序也读写**自己所在目录**的
   > `config.json`。放在一起才能共用同一份配置。

   或者直接用 `package.cmd` 产出的绿色包，那里面两个 exe 本来就在一起。

2. 运行 `RemapCopilotKey.exe`（配置程序，会弹一次 UAC），选择映射目标，点「保存」。
3. **不要点「注册服务」** —— 开机自启目前不可用（原因见下）。每次开机后手动点一次
   「启动服务」即可，或者直接双击 `RemapCopilotKeyService.exe`。

   > 如果你之前注册过自启任务，先点一次「结束并清除服务」把它删掉：
   > 留着它会让「启动服务」改走计划任务，结果启动出一个**收不到按键**的实例。

也可以直接用命令行操作服务程序：

```bat
RemapCopilotKeyService.exe                  启动服务（后台静默运行）
RemapCopilotKeyService.exe --stop           停止正在运行的实例
RemapCopilotKeyService.exe --config <路径>  使用指定的配置文件
RemapCopilotKeyService.exe --loglevel debug 临时覆盖日志级别
RemapCopilotKeyService.exe --quiet          不弹出任何对话框
RemapCopilotKeyService.exe --help           查看全部参数
```

> 服务程序**不提供** `--install` / `--uninstall` / `--status`：
> 开机自启的注册与删除在配置程序里，请用界面上的按钮。

**服务程序与配置程序是两个完全独立的进程**：服务负责拦截与执行，配置程序只负责写
`config.json`。关掉配置程序不影响服务运行，服务也不依赖配置程序即可工作。

### 配置程序的四个按钮

| 按钮 | 作用 |
|---|---|
| **启动服务** | 未注册自启时**直接启动**服务程序 —— 会弹一次 UAC，但这是目前**唯一能正常工作**的启动方式。已注册自启时会改走计划任务，而那条路是坏的（低完整性，钩子收不到按键） |
| **停止服务** | 让正在运行的服务退出（走窗口消息） |
| **注册服务** | 注册登录自启计划任务。**目前不推荐**：任务拉起的实例是低完整性，钩子收不到按键，详见下文 |
| **结束并清除服务** | 停止服务 + 删除开机自启注册，Copilot 键恢复原始行为 |

> 配置程序本身需要管理员权限（双击会弹一次 UAC），这是它能在本进程内注册计划任务、
> 以及与服务程序保持同级权限的前提。

### 快捷键的录制方式

按键映射与组合键都是**点击方框后直接按键盘**来录制的：

- 空框显示 `<空白>`
- 点击进入捕获后，框内显示 `<按下键盘任意按键捕获>`，**原有按键会被先清空**
- **捕获到一个键后自动退出捕获状态**
- 组合键是**三个独立的方框**（`框 + 框 + 框`），最多填三个，也可以只填一到两个
- 想中途放弃就点「取消捕获」，想直接抹掉已录的键就点「清空」
- **会区分左右修饰键**：录左 Ctrl 显示「左 Ctrl」，录右 Ctrl 显示「右 Ctrl」
- **Win 键也可以录**：捕获期间会临时挂一个低级键盘钩子拿到 LWin / RWin
  （Windows 键在到达普通窗口之前就被系统处理掉了，普通按键事件收不到它）。
  代价是**按 Win 键时开始菜单仍会弹出** —— 钩子只有捕获那一刻存在，录完立刻卸掉。
- **自动屏蔽非法输入**：只接受键表里真实存在的实体键，`0x15`（KANA）这类输入法伪键码录不进来

想映射 `Win + 某键` 的组合：录一个 `左 Windows` 到组合键的第一个框，再录后面的键即可。
也可以使用「Windows 实用功能」里的预置项，或在 `config.json` 里手写 `VK_LWIN`。

### 关闭时的未保存提醒

界面内容与已保存的 `config.json` 不一致时，关闭窗口会提示：

- **是** —— 保存后退出（校验不通过会留在窗口里继续改）
- **否** —— 不保存直接退出
- **取消** —— 返回继续编辑

### 打开应用支持的格式

文件选择框接受 Windows 上所有常见可执行类型与快捷方式：
`*.exe` `*.com` `*.bat` `*.cmd` `*.lnk` `*.msc` `*.ps1` `*.vbs` `*.js` `*.jar` `*.py`，
也可以选「所有文件」。`*.lnk` 快捷方式交给 `ShellExecute` 直接打开。

### 管理员权限开关的含义

界面上的「以管理员权限运行（被打开的程序）」属于 **打开应用程序** 动作：

- **勾选** → 被打开的程序以管理员权限启动（`ShellExecute` 的 `runas` 动词，会弹一次 UAC）
- **不勾选** → 以**登录用户的普通权限**启动

不勾选时服务程序不会把浏览器等程序提升起来：它先取 shell（explorer）进程的令牌，
如果那是提权令牌就换成它的受限令牌，再用 `CreateProcessWithTokenW` 启动，
这样浏览器不会报"现有实例正在以提升的权限运行"。

**服务程序自身固定以管理员权限运行**（清单里是 `requireAdministrator`），
因为低级键盘钩子受 UIPI 限制，非提升进程看不到送往提升窗口的输入。

## 配置文件格式

`config.json`，UTF-8 无 BOM，放在程序目录。完整说明见
[docs/配置格式.md](docs/配置格式.md)。

```json
{
  "version": 1,
  "language": "zh-CN",
  "logLevel": "info",
  "enabled": true,

  "mapping": {
    "trigger": "copilotKey",
    "reloadOnChange": true,
    "action": { "type": "sendKey", "vkey": "VK_RCONTROL" }
  }
}
```

`mapping.action` 的五种写法：

```jsonc
// 映射为按键（按住跟随）
{ "type": "sendKey", "vkey": "VK_RCONTROL" }

// 映射为组合键（触发一次）
{ "type": "sendChord", "keys": ["VK_CONTROL", "VK_SHIFT", "VK_ESCAPE"] }

// 打开应用程序
{ "type": "openApp", "path": "C:\\Windows\\System32\\notepad.exe", "args": "" }

// 打开网页
{ "type": "openUrl", "url": "https://github.com/Anlerway/RemapCopilotKey" }

// Windows 实用功能
{ "type": "systemAction", "action": "taskManager" }
```

可用按键名以 `VK_` 开头（`VK_RCONTROL`、`VK_F13`、`VK_OEM_PERIOD`、`VK_MEDIA_PLAY_PAUSE` …），
也可以省略前缀或使用别名（`RCONTROL`、`CTRL`、`ESC`、`ENTER` …）。
完整清单见 [docs/配置格式.md](docs/配置格式.md)。

**如果手改配置写错了**，服务程序不会拒绝启动：它会记一条警告日志、把出错的那一项
回退成默认值继续运行。

`logLevel` 默认是 **`error`（只记错误）**，正常使用时日志文件几乎不增长；
排查问题前先把它改成 `debug`。

## 开机自启：目前不可用

> **结论先放这里：计划任务拉起来的服务实例是低完整性（0x1000），
> 低级键盘钩子收不到任何按键。开机自启这条路目前走不通。**

不是没查过 —— 四条路都试完了，根因是同一个，下面如实记录。

### 实测证据

`RemapCopilotKey.boot.log`（它**不受 `logLevel` 影响**：默认 `logLevel=error` 时普通日志
几乎什么都不写，而这里恰恰是最需要现场记录的地方）：

```
21:40:39 进程启动 pid=11644 模式=run
21:40:39 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=低(0x1000) 提权=是
21:40:46 进程启动 pid=24980 模式=run
21:40:46 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=低(0x1000) 提权=是
21:40:54 进程启动 pid=25328 模式=run
21:40:54 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=高(0x3000) 提权=是
```

前两次是**计划任务启动**，第三次是**直接启动**。注意前两次那对组合：

**`提权=是` 但 `完整性=低(0x1000)`。**

这不是 UAC 的过滤令牌 —— 过滤令牌的 `TokenIsElevated` 是 `FALSE`，`提权` 会显示"否"。
这是计划任务程序发出的**受限令牌**：进程确实提权了，却被压在低完整性上，于是低级键盘钩子
受 UIPI 限制**一个按键都收不到** —— 钩子装得上、日志照写、没有任何报错。

### 四条路都失败了

| 方案 | 结果 |
|---|---|
| `schtasks /Create /SC ONLOGON /TR ... /RL HIGHEST`（不带 `/RU`） | ❌ 低完整性 |
| 任务定义里写 `<RequiredPrivileges>` | ❌ **注册直接失败**：v1.3 元素，主体是登录用户时校验器拒绝（`(20,57):Privilege:SeImpersonatePrivilege`） |
| 主体 `GroupId` + 显式 `TASK_LOGON_INTERACTIVE_TOKEN` | ❌ 登录方式被**强制改写**成 `TASK_LOGON_GROUP`，低完整性 |
| 主体 `UserId`（当前用户 SID）+ `TASK_LOGON_INTERACTIVE_TOKEN` + `HighestAvailable` | ❌ **仍是低完整性**（上面实测的前两行） |
| 启动时触发 + 主体 **SYSTEM** + Session 0 宿主 + `CreateProcessAsUser` 投递 | ❌ SYSTEM 任务同样是低完整性受限令牌 |

最后一条的实测现场：

```
宿主身份: 账户=NT AUTHORITY\SYSTEM 会话=0 ... 完整性=低(0x1000) 提权=是
特权: SeTcb=1 SeAssignPrimaryToken=1 SeIncreaseQuota=1
WTSQueryUserToken(会话 1) 失败：错误码 5      ← ERROR_ACCESS_DENIED
```

账户是 SYSTEM、三个特权也确实打开了，`WTSQueryUserToken` 照样拒绝访问 ——
受限令牌下调不动它。

**共同点：只要进程是计划任务拉起来的，拿到的就是受限令牌。**
主体的选择（当前用户 / SYSTEM）、登录方式（`InteractiveToken` / `ServiceAccount`）、
运行级别（`HighestAvailable`）都改变不了这一点。

### 现在该怎么用

1. 打开配置程序（会弹一次 UAC），配置好映射，点「保存」。
2. 点「**结束并清除服务**」，把自启任务删掉 —— 留着它会让「启动服务」改走计划任务，
   启动出一个收不到按键的实例。
3. 之后每次开机后，手动点一次「**启动服务**」，或者直接双击
   `RemapCopilotKeyService.exe`。这条路径的完整性是 `高(0x3000)`，功能正常，
   代价只是每次要确认一次 UAC。

命令行同样可以直接启动（一样会弹一次 UAC）：

```bat
RemapCopilotKeyService.exe
```

### 想继续折腾的话：还没排除的方向

按可能性排序，都尚未验证：

1. **换成 SCM 注册的真正 Windows 服务**（LocalSystem、自动启动）。
   服务进程的令牌由 SCM 用服务登录方式创建，不经过计划任务那套硬化 ——
   这是目前唯一还没试过、且理论上能拿到完整 SYSTEM 令牌的载体。
   代价是要新增装 / 卸服务的代码和安装脚本。
2. **服务程序清单里的 `requireAdministrator` 是否触发了硬化。**
   同类的 NoCopilotKey 主程序**没有清单**（`asInvoker`），它的计划任务据称可用。
   把服务程序的清单去掉再试一次成本很低，但那样它就只能以**中完整性**运行，
   送往提升窗口的输入仍然拦不到。
3. **这台机器上是否有组策略 / SRP / AppLocker 在影响任务令牌。**
   换一台干净机器复现一次，就能区分"Windows 的普遍行为"和"本机策略造成的"。

### 怎么判断到底成没成

**"注册成功"完全不作数** —— 上面第 1、3、4 条都是静默失败。只看 `boot.log` 里那一行：

```
运行环境: ... 完整性=高(0x3000) 提权=是
```

`AutoStart.Install` 也会在注册后**回读** `Principal.LogonType`，不是 `InteractiveToken`
就删掉任务并明确报错 —— 但那只能保证任务定义正确，**保证不了令牌完整性**，
这一点已经被实测推翻过一次。

想看系统实际存下来的任务定义，用诊断开关导出（同时列出关键字段）：

```
RemapCopilotKey.exe --export-task task.xml
```

## 多语言

语言包是嵌入资源 `src/Configurator/Lang/*.json`：

```json
{
  "code": "zh-CN",
  "name": "简体中文",
  "strings": { "app.title": "RemapCopilotKey 配置程序" }
}
```

新增语言：复制一份改 `code` / `name` 并翻译 `strings`，重新构建即可，**不需要改代码**。
`Configurator.Tests` 会断言所有语言包的键集完全一致，漏翻会直接测试失败。

服务程序的对话框也做了中英双语，跟随配置里的 `language` 字段。

## 测试与验证

```bat
build.cmd test
```

会依次运行：

| 测试 | 断言数 | 内容 |
|---|---|---|
| `ConfigContractTests`（C#） | 126 | JSON 字段名、往返一致、校验逻辑、键表、语言包完整性、自带 JSON 层；并写出一份样例配置 |
| `json_test`（C++） | 68 | 自带 JSON 解析器的语法、转义、UTF-8、代理对、错误定位、深度限制 |
| `config_test`（C++） | 164 | 键名往返、配置解析、各类动作、异常回退、UTF-8 路径，**并读回 C# 写出的那份配置逐项核对** |

C# 写 → C++ 读的闭环是这个项目最容易出问题的地方（字段名一改就全盘失效），
所以专门做了跨语言验证。

单独验证配置程序写出的 JSON：

```bat
build\out\Configurator\RemapCopilotKey.exe --dump-default-config test.json
```

## 已知限制

- **开机自启目前不可用**：计划任务拉起的实例是低完整性（0x1000），钩子收不到任何按键。
  每次开机后需要**手动启动**一次（会弹一次 UAC）。
  详见[开机自启：目前不可用](#开机自启目前不可用)。
- **必须运行在交互式会话**，不能直接注册成 Windows 服务（Session 0 收不到交互桌面的按键）。
- **安全桌面覆盖不到**：Ctrl+Alt+Del、UAC 提示框、登录界面的按键，用户态钩子拿不到。
  要覆盖这些场景只能上键盘过滤驱动，那需要微软签名的驱动（见[相关背景](#相关背景)）。
- **键盘钩子是链式的**：多个程序都装低级钩子时，后装的先看到按键。若遇到远程桌面等
  程序抢先处理，可以重启服务重新抢到链首。
- **非管理员运行时**，送往管理员窗口的按键拦不到。程序会在启动时记一条警告。
- **序列识别有 100ms 窗口**：极慢的蓝牙键盘上，如果三键之间超过 100ms，会被判定为
  用户手按了 Win 和 Shift，此时 Copilot 键不会触发（但正常按键不受影响）。
- 只支持 64 位 Windows。

## 故障排查

**先看两个日志**（都在程序目录）：

| 文件 | 内容 |
|---|---|
| `RemapCopilotKey.boot.log` | 启动追踪，**不受 `logLevel` 影响**。排查"自启没生效""钩子收不到按键"从它开始 |
| `RemapCopilotKeyService.log` | 普通日志，级别由 `logLevel` 决定（默认只记 `error`） |

| 现象 | 排查方向 |
|---|---|
| 按 Copilot 键没反应 | 配置里 `enabled` 是否为 `true`；`boot.log` 里那行 `完整性=` 是不是 `高(0x3000)`；把 `logLevel` 改成 `debug` 看完整的按键流 |
| 变成 `Win+Shift+…` 了 | 说明序列没凑齐，多半是三键间隔超过 100ms（蓝牙键盘），或钩子被别的程序抢先 |
| Ctrl 卡住不放 | 常见于切到管理员窗口。确认服务是以管理员身份运行的；或重启服务重新挂钩子 |
| 改动配置没生效 | 确认配置程序和服务程序在**同一个目录**；或点「保存」让配置程序发重载消息 |
| 管理员窗口里不生效 | 服务必须以管理员身份运行，且要在**同一个登录会话**里 |
| 注册了自启，开机后按 Copilot 键却没反应 | **已知问题**：计划任务启动的实例是低完整性(0x1000)，钩子收不到按键。看 `boot.log` 里的 `完整性=`；先点「结束并清除服务」删掉任务，再**手动**「启动服务」 |
| 装了自启但服务根本没起来 | 到"任务计划程序"里找 `RemapCopilotKey`；`--export-task` 看它的登录方式与运行级别；或点一次「启动服务」看它是否报错 |
| 点「注册服务」报权限不足 | 以管理员身份运行配置程序：注册计划任务必须要管理员权限 |

## 开发约定（踩过的坑）

这几条都是本项目实际踩过的，改动时请留意：

1. **`.cmd` / `.bat` 必须是纯 ASCII + CRLF**。`cmd.exe` 用 OEM 代码页解析批处理，
   中文注释的 UTF-8 字节会和后面的结构字符配成双字节字符，把 `{`、`)` 吃掉，
   脚本直接语法错乱。`.gitattributes` 已强制 `eol=crlf`。
2. **`.ps1` 必须带 UTF-8 BOM**。否则 Windows PowerShell 5.1 按 GBK 解码，
   中文注释同样会吃掉结构字符。`build.ps1` 出现 `Missing closing '}'` 就是这个原因。
3. **链接器要加 `/MANIFESTUAC:NO`**。否则自带的 `app.manifest` 和 MSVC 默认生成的
   UAC 片段会因 `level` 不一致而让 `mt.exe` 报 `c1010001`。
4. **Windows 头文件不定义 `VK_A`..`VK_Z`**，字母键的虚拟键码就是其大写 ASCII 值。
5. **DPI 设置**：WPF 是分辨率无关的，高 DPI 只在 `app.manifest` 里声明
   `dpiAware` / `dpiAwareness=PerMonitorV2` 即可。
6. **C++ 源码按 UTF-8 无 BOM 保存**，编译时加 `/utf-8`。
7. **C# 源码按 UTF-8 无 BOM 保存**；`.xaml` 同样无 BOM；但 `.ps1` **必须带 BOM**（见第 2 条）。

更多细节见 [docs/开发笔记.md](docs/开发笔记.md)。

## 关于"未知发布者"

本项目**不对产物签名**。未签名的 exe 表现取决于它是否请求提权：

| exe | 清单 | 双击运行 |
|---|---|---|
| `RemapCopilotKey.exe`（配置程序） | `requireAdministrator` | 请求提权 → UAC 弹窗里显示"未知发布者" |
| `RemapCopilotKeyService.exe`（服务） | `requireAdministrator` | 同上 |

**注意：「未知发布者」只是 UAC 提权弹窗里的一个署名栏**，不是"运行未签名程序"的通用警告。
不请求提权的程序永远不会显示它 —— 但本项目两个 exe 都需要管理员权限，所以都会看到。

想避开那个字样有两条路，**但其中一条目前走不通**：

1. ~~用计划任务自启~~ —— **这条现在不成立**：任务确实能静默提权启动、完全不经过 UAC，
   但启动出来的实例是**低完整性**，钩子收不到按键
   （见[开机自启：目前不可用](#开机自启目前不可用)）。
2. **购买受信任 CA 签发的代码签名证书（OV / EV）** 并签名 —— 目前只剩这一条。

> 自签名证书只能在你装过该证书的机器上生效，对下载你程序的人无效，
> 所以仓库里**没有**提供签名脚本。

## 相关背景

排查"能不能复用已有签名驱动"时得到的结论（本项目**没有**采用）：

- 唯一通用的现成方案是 [Interception](https://github.com/oblitum/Interception) 键盘过滤驱动。
  它确实是驱动级的（UAC 桌面、登录界面都能覆盖），但：
  - 最后一个 release 是 **v1.0.1 / 2017 年**，用 **WDK 7.1** 构建，只声明测试到 Windows 10；
  - 驱动签名来自作者个人的 GlobalSign 代码签名证书（**2012 年就过期的交叉签名**），
    不是微软 WHQL / attestation 签名；
  - 已知问题：客户端崩溃会让键盘鼠标失效到重启为止；在 Secure Boot 机器上安装会失败。
- 因此本项目选择用户态钩子方案作为主线。

---

**如果这个项目帮到了你，欢迎到 <https://github.com/Anlerway/RemapCopilotKey> 点个 Star。**
