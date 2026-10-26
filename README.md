<p align="center">
  <img src="docs/images/banner.png" alt="RemapCopilotKey" width="760">
</p>

<p align="center">
  <strong>中文</strong> | <a href="README.en.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/Anlerway/RemapCopilotKey/releases/latest"><img alt="release" src="https://img.shields.io/github/v/release/Anlerway/RemapCopilotKey?style=flat-square&color=6c5ce7"></a>
  <a href="https://github.com/Anlerway/RemapCopilotKey/stargazers"><img alt="GitHub stars" src="https://img.shields.io/github/stars/Anlerway/RemapCopilotKey?style=flat-square&color=eab308"></a>
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-4b6fff?style=flat-square">
  <a href="LICENSE"><img alt="license" src="https://img.shields.io/badge/license-AGPL--3.0-22c55e?style=flat-square"></a>
</p>

# RemapCopilotKey

把笔记本上被微软顶掉的 **Copilot 键**改回真正好用的**右 Ctrl**，或者映射成任意按键、组合键、打开程序、打开网页、Windows 实用功能。

不刷固件、不改 `Scancode Map`、不装驱动：一个用户态低级键盘钩子做三键**序列识别**，配置是一份 JSON。

**适用**：键盘上带 Copilot 键、又用得上右 Ctrl 的人。微软要求没有多余键位的键盘用 Copilot 键替换右 Ctrl，BIOS 和 Windows 设置里都没有开关，而 Copilot 键在固件层上报的其实是 `左Win + 左Shift + F23` 三个真实按键 —— 所以任何"只换 F23 扫描码"的办法都会留下 `Win+Shift`。

- 五种映射目标：按键（按住跟随）、三连组合键、打开程序、打开网页、14 个 Windows 实用功能
- 只认**三键序列**：单独按 Win、单独按 Shift、`Win+E`、`Win+Shift+S` 都照原样工作，不会被"重映射"搞坏
- 改完即生效：配置放在程序目录，服务端 2 秒轮询热加载；界面**没有保存按钮**，停手 600 ms 自动写盘
- 开机自启 = LocalSystem 服务 + 会话投递，钩子进程完整性 `高(0x3000)`，**全程不弹 UAC**
- 零外部依赖：C++ 侧自带 JSON 解析，界面是 .NET Framework 4.8（Win10 1903+ 自带），发布包不到 0.5 MB

## 界面介绍

<p align="center">
  <img src="docs/images/main-window.png" width="600" alt="主界面：服务管理在最上方，底部常驻保存状态">
</p>

<p align="center"><em>主界面：注册服务是主按钮，没有保存按钮，底部那行就是保存状态</em></p>

点「注册服务」成功后弹这句提示：

<p align="center">
  <img src="docs/images/register-dialog.png" width="520" alt="注册成功，重启后会自动启动">
</p>

注册完顶部状态条变绿，钩子进程已经被投递进当前会话：

<p align="center">
  <img src="docs/images/service-registered.png" width="600" alt="已注册：服务运行中，开机自启已注册">
</p>

## 安装

### 快速开始（推荐）

1. 从 [Releases](https://github.com/Anlerway/RemapCopilotKey/releases) 下载 `RemapCopilotKey-v1.0.0-x64.zip`（或自己 `package.cmd` 打一个）
2. 解压到**同一个目录**，例如 `C:\Program Files\RemapCopilotKey\`

   > 两个 exe 必须放在一起：配置程序与服务程序各自读写**自己所在目录**的 `config.json`。

3. 双击 `RemapCopilotKey.exe`（会弹一次 UAC），选好映射目标 —— 改完就自动保存了
4. 点「**注册服务**」：注册成 LocalSystem 自动启动服务并立即启动，此后每次开机都会把钩子进程投递进你的会话，**不再弹 UAC**

用安装包（`package.cmd -Installer`）装的用户第 4 步已经自动做了，卸载时会自动 `--unregister-service`。

### 更新

覆盖 exe 即可，配置在 `config.json` 里不会被动。服务正在跑就先「停止服务」，覆盖后再「启动服务」。

### 卸载

点「**结束并清除服务**」：停服务 → 删 SCM 注册 → 确认服务名消失 → 清掉旧版计划任务，Copilot 键恢复原始行为；然后删目录即可。

手动清理：

```bat
sc stop RemapCopilotKeyAgent
sc delete RemapCopilotKeyAgent
schtasks /Delete /TN RemapCopilotKey /F
```

### 从源码构建

环境：Visual Studio（含 MSVC 与 .NET Framework 4.8 Targeting Pack）+ Windows SDK，不需要 .NET SDK 与 NuGet 包。

```bat
build.cmd               :: 编译 + 跑全部测试
build.cmd test          :: 只跑测试
package.cmd             :: 生产版绿色包 + SHA256SUMS.txt（加 -Installer 出安装程序，需 Inno Setup）
```

## 功能

| 目录 | 操作 |
| --- | --- |
| 映射为按键 | 按住 Copilot 时目标键保持按下，松开一起松开；默认右 Ctrl |
| 映射为组合键 | 按一下发一次三连键，如 `Ctrl + Shift + Esc` |
| 打开程序 | 选 exe / 快捷方式 / 脚本，可带参数，可勾选以管理员运行 |
| 打开网页 | 交给默认浏览器 |
| Windows 实用功能 | 资源管理器、任务管理器、cmd、PowerShell、截图、截图工具、设置、控制面板、计算器、记事本、任务视图、剪贴板历史、表情面板、锁定屏幕（已在跑的会再按一次关掉） |
| 录快捷键 | 点方框直接按键盘录制，区分左右修饰键，Win 键也能录（录制时开始菜单仍会弹） |
| 自动保存 | 停手 600 ms 写盘并通知服务重载；校验不过只提示不写盘 |
| 恢复默认 | 底部「恢复默认配置」，确认后整体回到出厂默认 |
| 服务管理 | 注册 / 结束并清除 / 启动 / 停止，按钮可用状态跟着真实状态走 |
| 多语言 | 中英语言包，`Lang\` 里丢一个 JSON 就能加语言（测试会断言键集一致） |

## 配置

`config.json`（UTF-8 无 BOM）放在程序目录。服务端每 2 秒检查一次时间戳与大小，改完保存即生效：

```json
{
  "version": 1,
  "language": "zh-CN",
  "logLevel": "error",
  "enabled": true,
  "mapping": {
    "trigger": "copilotKey",
    "reloadOnChange": true,
    "action": { "type": "sendKey", "vkey": "VK_RCONTROL" }
  }
}
```

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `language` | `"zh-CN"` | 界面语言，对应 `Lang\` 下的语言包 |
| `logLevel` | `"error"` | `off` / `error` / `warn` / `info` / `debug`，排查问题先改 `debug` |
| `enabled` | `true` | 总开关；关掉后钩子仍安装但不拦截任何按键 |
| `mapping.trigger` | `"copilotKey"` | 目前只有这一种触发方式 |
| `mapping.reloadOnChange` | `true` | 配置文件变化时自动重载 |
| `mapping.action` | `sendKey` → `VK_RCONTROL` | 五种动作写法见下表 |

| 动作 | 写法 |
| --- | --- |
| 映射为按键 | `{ "type": "sendKey", "vkey": "VK_RCONTROL" }` |
| 映射为组合键 | `{ "type": "sendChord", "keys": ["VK_CONTROL", "VK_SHIFT", "VK_ESCAPE"] }` |
| 打开程序 | `{ "type": "openApp", "path": "C:\\Windows\\System32\\notepad.exe", "args": "" }` |
| 打开网页 | `{ "type": "openUrl", "url": "https://github.com/Anlerway/RemapCopilotKey" }` |
| 实用功能 | `{ "type": "systemAction", "action": "taskManager" }` |

按键名以 `VK_` 开头，可以省略前缀或使用别名（`RCONTROL`、`CTRL`、`ESC`、`ENTER` …），
完整清单 147 项见 [设计说明 · 按键名清单](docs/设计说明.md#按键名清单)。

**手写错了不会让服务起不来**：非法的那一项回退成默认值并记一条警告日志。

## 命令行

服务程序：

| 参数 | 说明 |
| --- | --- |
| （无） / `--run` | 以钩子模式启动（后台静默，无托盘无窗口） |
| `--service` | 服务宿主模式，只给 SCM 用；手动运行会因为"不是由 SCM 启动"直接退出 |
| `--stop` | 停止正在运行的实例 |
| `--config <路径>` | 使用指定配置文件 |
| `--loglevel <级别>` | 临时覆盖日志级别 |
| `--quiet` / `--help` | 不弹任何对话框 / 显示用法 |

配置程序：`--register-service`、`--unregister-service`（安装包用）、`--dump-default-config <路径>`、`--diagnose`。

## 开机自启是怎么做的

一句话：**注册一个 LocalSystem 的自动启动 Windows 服务，由它把钩子进程投递进你的登录会话。**

- Session 0 里的服务宿主用 `WTSQueryUserToken` + `DuplicateTokenEx`（并取关联的提权令牌）拿到你的令牌，
  再用 `CreateProcessAsUserW(winsta0\default)` 把 `--run` 的钩子进程投递进交互会话；
- 钩子进程的完整性是 `高(0x3000)`，**不经过 UAC**；它常驻你的会话，自己装键盘钩子；
- 判断成没成只看一处：`RemapCopilotKey.boot.log` 里钩子进程那行必须是 `完整性=高(0x3000)`、`会话` 不为 0。

为什么不能用计划任务、为什么钩子进程不能自己当服务，见[设计说明 §8](docs/设计说明.md#8-提权与开机自启)。

## 文档

- [设计说明](docs/设计说明.md)：总体架构、序列状态机、提权与投递、界面与取舍
- [配置文件格式](docs/设计说明.md#15-附录配置文件格式) · [命令行参数](docs/设计说明.md#16-附录命令行参数) · [按键名清单](docs/设计说明.md#按键名清单)
- [界面截图](docs/images/)

## 开发与测试

```bat
build.cmd test
```

| 测试 | 断言数 | 内容 |
| --- | --- | --- |
| `ConfigContractTests`（C#） | 126 | 字段名、往返一致、校验逻辑、键表、语言包键集完整、自带 JSON 层 |
| `json_test`（C++） | 68 | 自带 JSON 解析器的语法、转义、UTF-8、代理对、错误定位 |
| `config_test`（C++） | 164 | 键名往返、配置解析、各类动作、异常回退，并读回 C# 写出的配置逐项核对 |

C# 写 → C++ 读的跨语言闭环是这里最容易出问题的地方（改个字段名就全盘失效），所以专门做了验证。

改动时留意几条踩过的坑：

1. `.cmd` / `.bat` 必须是纯 ASCII + CRLF —— `cmd.exe` 用 OEM 代码页解析批处理，中文注释会吃掉结构字符；
2. `.ps1` 必须带 UTF-8 BOM —— PowerShell 5.1 按 GBK 解码，同样会报 `Missing closing '}'`；
3. C++ 链接器要加 `/MANIFESTUAC:NO`，否则自带 `app.manifest` 与 MSVC 默认的 UAC 片段冲突，`mt.exe` 报 `c1010001`；
4. Windows 头文件不定义 `VK_A`..`VK_Z`，字母键的虚拟键码就是其大写 ASCII 值；
5. C++ / C# / `.xaml` 源码按 UTF-8 无 BOM 保存，C++ 编译加 `/utf-8`。

## 已知限制

- **安全桌面覆盖不到**：Ctrl+Alt+Del、UAC 提示框、登录界面的按键，用户态钩子拿不到。
- **钩子进程必须跑在交互式会话**：能常驻 Session 0 的只有服务宿主，它自己不装钩子。
- **非管理员运行时**拦不到送往管理员窗口的按键（启动时会记一条警告）。
- **序列识别有 100 ms 窗口**：蓝牙键盘若三键间隔超过 100 ms，会被当成手按了 Win 和 Shift。
- **键盘钩子是链式的**：多个程序都装低级钩子时后装的先看到按键；被抢了重启服务即可。
- 只支持 64 位 Windows 10 / 11。

## 故障排查

先看程序目录里的两个日志：`RemapCopilotKey.boot.log`（启动追踪，**不受 `logLevel` 影响**，排查自启与钩子从这里开始）、`RemapCopilotKeyService.log`（普通日志，默认只记 error）。

| 现象 | 排查方向 |
| --- | --- |
| 按 Copilot 键没反应 | `enabled` 是否为 `true`；`boot.log` 那行 `完整性=` 是不是 `高(0x3000)`；`logLevel` 改 `debug` 看按键流 |
| 变成 `Win+Shift+…` | 序列没凑齐：三键间隔超过 100 ms（蓝牙键盘），或钩子被别的程序抢先 |
| Ctrl 卡住不放 | 常见于切到管理员窗口；确认服务以管理员身份运行，或重启服务重新挂钩子 |
| 改配置没生效 | 两个 exe 是否在同一目录；底部是不是「已自动保存」；手改的可以点「重新读取」 |
| 开机后按了没反应 | `boot.log` 里钩子那行的 `完整性` 与 `会话`；不对就「结束并清除服务」再「注册服务」 |
| 点注册报权限不足 | 以管理员身份运行配置程序（调 SCM 需要管理员权限，错误 5） |

## issue

遇到 bug 或者想新增映射动作，欢迎提 issue：
<https://github.com/Anlerway/RemapCopilotKey/issues>

## 关于"未知发布者"

本项目不对产物签名，两个 exe 都是 `requireAdministrator`，所以**手动双击**与**首次注册服务**时 UAC 里会显示"未知发布者"。
开机自启不经过 UAC，不会再看到它。想彻底消掉只能买受信任 CA 的代码签名证书（自签名对别人的机器无效，所以仓库不提供签名脚本）。

## 许可证

AGPL-3.0，见 [LICENSE](LICENSE)。
