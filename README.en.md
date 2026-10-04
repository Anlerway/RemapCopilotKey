# RemapCopilotKey

> **Repository: <https://github.com/Anlerway/RemapCopilotKey>** ｜ 中文: [README.md](README.md)

Turns the **Copilot key** that Microsoft forces onto new laptops (it is just
`LeftWin + LeftShift + F23`) back into a real, usable **Right Ctrl** — or maps it to any
key, key combination, application, URL or Windows system action.

Configuration is JSON and the GUI is localised.

> ### ⚠️ Current status: auto-start is **not working**
>
> **Please read this first, so you do not configure it only to find out it does nothing.**
>
> A service instance started by the scheduled task runs at **low integrity (0x1000)**, so the
> low-level keyboard hook is blocked by UIPI and **receives no keys at all**. The only path that
> works today is **starting it directly** (double-click `RemapCopilotKeyService.exe`, or click
> **Start** in the configurator while auto-start is **not** registered): that one runs at
> **high integrity (0x3000)** and works — at the cost of one UAC confirmation each time.
>
> The measurements, all four failed approaches and the only workable usage are in
> [Auto-start: currently unavailable](#auto-start-currently-unavailable).

```
┌──────────────────────────┐        config.json        ┌───────────────────────────┐
│  Configurator (C# / WPF) │ ───────────────────────▶ │  Service (C++ / Win32)     │
│  · pick the target        │                          │  · global low-level kbd hook │
│  · multi-language         │ ◀── 2 s poll, hot reload │  · Copilot 3-key sequence    │
│  · writes config.json     │       window messages    │  · action execution / silent │
│  · registers auto-start   │       task starts it     │  · runs elevated             │
│  · runs elevated          │                          │                              │
└──────────────────────────┘                          └───────────────────────────┘
```

Both processes run **at the same elevation level** (administrator). The configurator owns
`config.json` and the auto-start registration; the service only intercepts keys.

---

## Contents

- [Why this exists](#why-this-exists)
- [Features](#features)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Installing and using](#installing-and-using)
- [Configuration file format](#configuration-file-format)
- [How auto-start works](#auto-start-currently-unavailable)
- [Localisation](#localisation)
- [Testing and verification](#testing-and-verification)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Development rules (lessons learned the hard way)](#development-rules-lessons-learned-the-hard-way)
- [About "Unknown publisher"](#about-unknown-publisher)
- [Background research](#background-research)

---

## Why this exists

Microsoft specifies the Copilot key's behaviour in
[Windows Copilot Key Guidance for Ecosystem Partners](https://learn.microsoft.com/en-us/legal/windows/hardware/copilot-key-guidance):

> It reports `make LWIN` → `make LSHIFT` → `make F23` on press, and
> `break F23` → `break LSHIFT` → `break LWIN` on release.
> Holding it repeats the three keys in Win / Shift / F23 order.
> This is normally implemented by the **embedded controller firmware** that reports keyboard input.

When a keyboard has no spare position, Microsoft **requires** the Copilot key to **replace
the right Ctrl key**. Neither the BIOS nor Windows offers a switch to change it back.

That has two consequences:

1. **No "blindly remap the scan code" solution can work.** The registry `Scancode Map`,
   SharpKeys and PowerToys' "remap a single key" can only touch F23 — they cannot remove the
   genuine LWin and LShift the firmware emits alongside it, so you end up with
   `Win+Shift+Ctrl+…`.
2. **Sequence detection is mandatory.** Only when those three keys appear in the right order,
   close together in time, can it be the Copilot key; otherwise the keys must be passed
   through untouched, or normal Win and Shift usage breaks.

This project does the second one.

## Features

- **Key sequence detection** — the full state machine, including the 100 ms timing window,
  hold-repeat, and replaying the suppressed keys in the correct order when a sequence breaks.
- **Never interferes with normal keys** — only the *combination* of Left Win, Left Shift and
  F23 triggers anything. Win alone, Shift alone, `Win+E`, `Win+Shift+S` and so on all behave
  exactly as before.
- **Five mapping targets**
  - a **key** (held down while the Copilot key is held — Right Ctrl by default)
  - a **chord**, **up to three keys** (e.g. `Ctrl + Shift + Esc`)
  - **launch an application** (with optional arguments)
  - **open a URL**
  - a **Windows system action**: File Explorer / Task Manager / cmd / PowerShell /
    Snipping Tool / Settings / Control Panel / Calculator / Notepad / Task View /
    Clipboard history / Emoji panel / Lock screen
- **JSON config with hot reload** — edit the file next to the program and it applies
  immediately, no restart.
- **Localised GUI** — Simplified Chinese, Traditional Chinese and English ship in the box;
  adding a language means dropping one JSON file into `Lang\`.
- **Completely silent** — the service has no tray icon and no window; failures go to a log file.
- **Press-to-record hotkey capture** — click the box and press the keys; no drop-down hunting.
- **Zero external dependencies** — the C++ side ships its own JSON parser.

> ❌ **Auto-start is currently unavailable**: an instance started by the scheduled task runs at
> low integrity, so the hook receives nothing. See
> [Auto-start: currently unavailable](#auto-start-currently-unavailable).

## How it works

### Service (C++)

A global low-level keyboard hook (`SetWindowsHookEx(WH_KEYBOARD_LL)`) feeds a state machine:

```
press    Idle ──LeftWin──▶ LeftWindows ──LeftShift──▶ LeftShift ──F23──▶ [fire]
release  Idle ──F23──▶ F23 ──LeftShift──▶ LeftShift ──LeftWin──▶ Idle
```

- While a sequence is in progress, LWin / LShift are **suppressed** rather than emitted, until
  it is clear whether this really is the Copilot key.
- Once all three keys arrive, the configured action runs and the suppressed LWin / LShift are
  **discarded** — never replayed.
- If the sequence breaks (another key down, any key up, or a 100 ms timeout), the suppressed
  keys are replayed in the correct order: **LWin → LShift → the key that broke it**.

Implementation notes that matter:

- **`SendInput` is never called inside the hook callback.** Hook callbacks are time-limited, and
  injected input re-enters our own hook. The hook only `PostMessage`s to a hidden message
  window; the window procedure performs the injection.
- **Injected input is let through by its `LLKHF_INJECTED` flag**, so it cannot feed back in.
- **Injection carries full scan codes and the extended-key flag**, with corrections for arrow
  keys, the numpad and PrintScreen, so games and DirectInput applications that only look at
  scan codes keep working.
- **Held keys are force-released when the foreground window becomes an elevated one.** A
  low-level hook in a non-elevated process cannot see input going to an elevated window (UIPI);
  if an injected Ctrl was down at that moment it would stick. The program watches foreground
  changes and sends the missing key-up immediately.
- **F23 is matched by both virtual key and scan code `0x6E`**, so it still works if the user
  has remapped F23 through the registry.

### Configurator (C# / WPF, .NET Framework 4.8)

- Only writes `config.json`; it takes no part in key interception.
- After saving it also posts a window message so the service reloads at once (the file watcher
  alone polls every 2 seconds).
- **Registers / removes the auto-start scheduled task** in-process through the Task Scheduler
  COM API (see `src/Configurator/AutoStart.cs`) — it does not shell out to `schtasks.exe`. The
  crucial part is the `TASK_LOGON_INTERACTIVE_TOKEN` argument of `RegisterTaskDefinition`, which
  exists only as a function argument and cannot be expressed by `schtasks`. See
  [How auto-start works](#how-auto-start-works).
- It runs **at the same elevation level** as the service (`requireAdministrator` in the
  manifest), which is why registering the task is never refused.

## Repository layout

```
RemapCopilotKey/
├─ RemapCopilotKey.sln             Solution (three projects)
├─ build.cmd / build.ps1           Unified build script
├─ package.cmd                     Thin wrapper for release packaging
├─ src/
│  ├─ Service/                     C++ service
│  │  ├─ app.manifest              requireAdministrator + DPI awareness
│  │  ├─ src/
│  │  │  ├─ main.cpp               Entry point, hidden message window, command line
│  │  │  ├─ CopilotHook.{h,cpp}    Keyboard hook + three-key sequence state machine
│  │  │  ├─ Actions.{h,cpp}        Action execution (inject keys / launch programs)
│  │  │  ├─ Config.{h,cpp}         Config model, load/save, hot-reload watcher
│  │  │  ├─ Keys.{h,cpp}           Virtual key names <-> codes
│  │  │  ├─ AutoStart.{h,cpp}      Elevation and environment queries (session / station / desktop)
│  │  │  ├─ Log.{h,cpp}            Logging (plus a logLevel-independent boot trace)
│  │  │  └─ Json.{h,cpp}           Bundled JSON parser / writer
│  │  └─ tests/
│  │     ├─ json_test.cpp          JSON module tests
│  │     └─ config_test.cpp        Config and key-name tests (incl. the cross-language contract)
│  ├─ Configurator/                C# configurator (WPF / net48)
│  │  ├─ App.xaml(.cs)             Entry point (--dump-default-config / --export-task / --diagnose)
│  │  ├─ MainWindow.xaml(.cs)      GUI
│  │  ├─ Assets/app.ico            Icon (GUI only; the service deliberately has none)
│  │  ├─ Themes/Styles.xaml        Light Windows 11 style global theme
│  │  ├─ HotkeyBox.cs              Press-to-record control (captures Win keys too)
│  │  ├─ KeyCatalog.cs             Key and system-action catalogue
│  │  ├─ AppConfig.cs              Config model (mirrors the C++ side)
│  │  ├─ Json.cs                   Minimal JSON reader/writer (net48 has no System.Text.Json)
│  │  ├─ AutoStart.cs              Auto-start task registration (Task Scheduler COM API)
│  │  ├─ Localization.cs           Localisation
│  │  ├─ ServiceControl.cs         Talking to the service
│  │  ├─ Diagnostics.cs            --diagnose startup log (a GUI has no stdout)
│  │  ├─ Compat.cs                 IsExternalInit for C# 9 records/init on net48
│  │  └─ Lang/{zh-CN,en-US}.json   Language packs
│  └─ Configurator.Tests/          C# config-contract tests (console)
├─ tools/
│  ├─ package.ps1                  Release packaging (portable zip + hashes + optional installer)
│  ├─ installer.iss                Inno Setup script
│  ├─ check-xml.ps1                Pre-build: reject illegal XML comments
│  └─ check-manifest.ps1           Post-build: verify the elevation manifest is embedded
├─ docs/                           Design notes (Chinese)
└─ .github/workflows/              CI: build + test + package
```

## Building

### Requirements

| Component | Version | Purpose |
|---|---|---|
| Visual Studio | 2019 or newer (with C++ build tools) | MSVC, MSBuild, and the .NET Framework 4.8 targeting pack |
| Windows SDK | 10.0.19041 or newer | |
| git | any | |

> **No .NET SDK and no NuGet are needed.** The configurator is a .NET Framework 4.8 WPF
> project built by the MSBuild that ships with Visual Studio, using the bundled .NET Framework
> 4.8 targeting pack. The script locates Visual Studio through `vswhere`, so you do **not**
> need to open a "VS Developer Command Prompt".

### Build

**With Visual Studio (recommended):** open `RemapCopilotKey.sln` and build `Release | x64`.
The solution contains three projects:

| Project | Type | Output |
|---|---|---|
| `RemapCopilotKeyService` | C++ / MSBuild | `RemapCopilotKeyService.exe` |
| `RemapCopilotKey` | C# / WPF (net48) | `RemapCopilotKey.exe` |
| `ConfigContractTests` | C# console tests | Config JSON contract tests |

**With the script:**

```bat
build.cmd
```

or directly through PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

### Build sub-commands

```bat
build.cmd service     build only the C++ service
build.cmd config      build only the C# configurator
build.cmd test        compile and run all unit tests (C# + C++)
build.cmd clean       delete build\
```

The build first runs an XML comment pre-check (`tools/check-xml.ps1`): a comment containing
two consecutive hyphens makes `app.manifest` illegal XML, and the resulting exe fails at
startup with nothing but *"The application has failed to start because its side-by-side
configuration is incorrect"* — no stack, no log. Neither MSBuild nor the XAML compiler checks
manifests, so the script does it.

After building the configurator it also runs `tools/check-manifest.ps1`, which verifies that
the elevation manifest is **really embedded in the exe**. Correct source does not imply a
correct binary — both checks exist because both mistakes were actually made.

Outputs (excluded by `.gitignore`):

```
build\out\Service\RemapCopilotKeyService.exe      <- service
build\out\Configurator\RemapCopilotKey.exe        <- configurator
artifacts\release\                                <- package.cmd release artefacts
```

### Release packaging

```bat
package.cmd
```

It builds, assembles the distribution folder, clears the mark-of-the-web, zips it, produces
SHA256 values and optionally builds an installer. Artefacts land in `artifacts\`:

| Artefact | Notes | Target machine needs |
|---|---|---|
| `release\RemapCopilotKey-v1.0.0-x64.zip` | **Portable, unzip and run** (~0.23 MB) | nothing at all |
| `release\RemapCopilotKey\` | The same folder before zipping | — |
| `release\SHA256SUMS.txt` | SHA256 of every artefact | — |
| `installer\RemapCopilotKey-v1.0.0-x64-setup.exe` | Installer (**optional**, ~2.2 MB) | — |

The portable zip contains just four files: the configurator, its `.exe.config`, the C++
service and a short readme. Build intermediates such as `.pdb` are never shipped.

Options:

```powershell
# Package existing build output without rebuilding; skip the installer
powershell -NoProfile -File tools\package.ps1 -SkipBuild -NoInstaller

# Set the version used in artefact names
powershell -NoProfile -File tools\package.ps1 -Version 1.1.0
```

The installer needs [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`ISCC.exe`).
**If it is missing the step is skipped, not failed** — the portable zip is the primary
distribution.

> **Why is there only one portable package, and why only 0.23 MB?**
> The configurator is a .NET Framework 4.8 WPF application, and net48 **ships with**
> Windows 10 1903+ / Windows 11, so no runtime has to be bundled.

> **About the mark-of-the-web:** the packaging script runs `Unblock-File`, but that only helps
> for files produced locally (inside a zip the mark survives unless the zip itself is unblocked
> before extraction). Users who download the zip may still see a security warning; only code
> signing really fixes that, and this project **does not sign** (see
> [About "Unknown publisher"](#about-unknown-publisher)).

## Installing and using

1. Build, then put `build\out\Service\RemapCopilotKeyService.exe` and everything from
   `build\out\Configurator\` into **one folder** (for example `C:\Program Files\RemapCopilotKey\`).

   > The configurator reads and writes the `config.json` **next to itself**, and so does the
   > service. They must share a folder to share one configuration.

   Or just use the portable zip produced by `package.cmd` — the two executables are already
   side by side there.

2. Run `RemapCopilotKey.exe` (the configurator; it asks for elevation once), pick a mapping and
   click **Save**.
3. **Do not click Register** — auto-start does not work today (see below). Click **Start** once
   after each boot instead, or simply double-click `RemapCopilotKeyService.exe`.

   > If you registered auto-start earlier, click **Remove** once to delete the task: while it
   > exists, **Start** goes through the scheduled task and launches an instance that
   > **receives no keys**.

The service can also be driven from the command line:

```bat
RemapCopilotKeyService.exe                  start the service (silent background app)
RemapCopilotKeyService.exe --stop           ask a running instance to exit
RemapCopilotKeyService.exe --config <path>  use a specific config file
RemapCopilotKeyService.exe --loglevel debug override the log level for this run
RemapCopilotKeyService.exe --quiet          never show a dialog
RemapCopilotKeyService.exe --help           show all options
```

> The service does **not** provide `--install` / `--uninstall` / `--status`; auto-start
> registration lives in the configurator, so use its buttons.

**The service and the configurator are entirely independent processes.** The service
intercepts and acts; the configurator only writes `config.json`. Closing the configurator does
not affect the service, and the service works fine without it.

### The four buttons

| Button | What it does |
|---|---|
| **Start** | With auto-start **not** registered, launches the service **directly** — one UAC prompt, but this is currently the **only way that works**. With auto-start registered it goes through the scheduled task instead, and that path is broken (low integrity, the hook receives no keys) |
| **Stop** | Asks the running service to exit (via a window message) |
| **Register** | Registers the logon auto-start task. **Not recommended right now**: the instance it starts runs at low integrity and its hook receives no keys — see below |
| **Remove** | Stops the service and deletes the auto-start registration, restoring the Copilot key's original behaviour |

> The configurator itself requires administrator rights (one UAC prompt on launch). That is
> what lets it register the task in-process and keeps it at the same elevation as the service.

### Recording hotkeys

Both the single key and the chord are recorded by **clicking the box and pressing keys**:

- An empty box shows `<empty>`.
- After clicking, the box shows `<press any key to capture>` and **the previous key is cleared**.
- **Capture ends automatically as soon as one key is captured.**
- A chord is **three separate boxes**; fill one to three of them.
- **Cancel capture** abandons it, **Clear** wipes what is recorded.
- **Left and right modifiers are distinguished**: left Ctrl shows as "Left Ctrl", right Ctrl as
  "Right Ctrl".
- **The Windows key can be recorded too.** During capture a low-level keyboard hook is installed
  temporarily, because Windows swallows the key before any ordinary window sees it. The cost is
  that **pressing Win still opens the Start menu** — the hook exists only while capturing and is
  removed immediately afterwards.
- **Invalid input is filtered out**: only real keys in the key table are accepted, so IME pseudo
  codes such as `0x15` (KANA) cannot be recorded.

To map `Win + something`, record `Left Windows` into the first chord box and then the other key
— or use one of the preset Windows actions, or write `VK_LWIN` into `config.json` by hand.

### Unsaved-changes prompt

If the GUI no longer matches the saved `config.json`, closing the window asks:

- **Yes** — save and exit (if validation fails you stay in the window)
- **No** — exit without saving
- **Cancel** — go back to editing

### File types accepted for "open application"

The file dialog accepts every common Windows executable type and shortcut:
`*.exe` `*.com` `*.bat` `*.cmd` `*.lnk` `*.msc` `*.ps1` `*.vbs` `*.js` `*.jar` `*.py`, plus
"All files". `*.lnk` shortcuts are handed straight to `ShellExecute`.

### What the elevation checkbox means

The **Run with administrator rights (the launched program)** checkbox belongs to the
**Open application** action:

- **Checked** — the target starts elevated (`ShellExecute` with the `runas` verb; one UAC prompt)
- **Unchecked** — the target starts with the **logged-on user's normal rights**

When it is unchecked the service deliberately avoids handing out its own elevation: it takes
the shell's (`explorer.exe`) token, swaps to its linked limited token if that one is elevated,
and launches through `CreateProcessWithTokenW`. That is what stops browsers from complaining
that "an existing instance is running with elevated privileges".

**The service itself always runs elevated** (`requireAdministrator` in its manifest), because a
low-level keyboard hook in a non-elevated process cannot see input going to elevated windows.

## Configuration file format

`config.json`, UTF-8 without BOM, in the program folder. Full reference (Chinese):
[docs/配置格式.md](docs/配置格式.md).

```json
{
  "version": 1,
  "language": "en-US",
  "logLevel": "info",
  "enabled": true,

  "mapping": {
    "trigger": "copilotKey",
    "reloadOnChange": true,
    "action": { "type": "sendKey", "vkey": "VK_RCONTROL" }
  }
}
```

The five shapes of `mapping.action`:

```jsonc
// A single key (held while the Copilot key is held)
{ "type": "sendKey", "vkey": "VK_RCONTROL" }

// A chord, fired once
{ "type": "sendChord", "keys": ["VK_CONTROL", "VK_SHIFT", "VK_ESCAPE"] }

// Launch an application
{ "type": "openApp", "path": "C:\\Windows\\System32\\notepad.exe", "args": "" }

// Open a URL
{ "type": "openUrl", "url": "https://github.com/Anlerway/RemapCopilotKey" }

// A Windows system action
{ "type": "systemAction", "action": "taskManager" }
```

Key names start with `VK_` (`VK_RCONTROL`, `VK_F13`, `VK_OEM_PERIOD`, `VK_MEDIA_PLAY_PAUSE`, …);
the prefix may be dropped and aliases are accepted (`RCONTROL`, `CTRL`, `ESC`, `ENTER`, …).

**A hand-edited mistake never stops the service.** It logs a warning, falls back to the default
for the offending item and keeps running.

`logLevel` defaults to **`error`**, so the log file barely grows in normal use. Turn it up to
`debug` before investigating a problem.

## Auto-start: currently unavailable

> **The short version: a service instance started by the scheduled task runs at low integrity
> (0x1000), so the low-level keyboard hook receives no keys. Auto-start does not work today.**

This is not for lack of trying — four approaches were implemented and measured, all failing for
the same underlying reason. The raw evidence follows.

### The measurements

From `RemapCopilotKey.boot.log` (it is **not affected by `logLevel`** — with the default
`logLevel=error` the ordinary log stays almost empty, and this is exactly the situation where
you most need a record):

```
21:40:39 进程启动 pid=11644 模式=run
21:40:39 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=低(0x1000) 提权=是
21:40:46 进程启动 pid=24980 模式=run
21:40:46 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=低(0x1000) 提权=是
21:40:54 进程启动 pid=25328 模式=run
21:40:54 运行环境: 账户=LAPTOP-DR4R4LO\ASUS 会话=1 窗口站=WinSta0 桌面=Default 完整性=高(0x3000) 提权=是
```

The first two were **started by the scheduled task**; the third was **started directly**. Note
the combination in the first two:

**`提权=是` ("elevated=yes") together with `完整性=低(0x1000)` ("low integrity").**

That is not the UAC filtered token — for a filtered token `TokenIsElevated` is `FALSE` and the
field would read `提权=否`. It is a **restricted token issued by the Task Scheduler**: the process
really is elevated, yet pinned to low integrity, so the low-level keyboard hook is blocked by
UIPI and **receives no keys at all** — the hook installs, the log looks healthy, nothing errors.

### All four approaches failed

| Approach | Result |
|---|---|
| `schtasks /Create /SC ONLOGON /TR ... /RL HIGHEST` (without `/RU`) | ❌ low integrity |
| `<RequiredPrivileges>` in the task definition | ❌ **registration fails outright**: a v1.3 element, rejected by the validator for a logged-on user principal (`(20,57):Privilege:SeImpersonatePrivilege`) |
| `GroupId` principal + explicit `TASK_LOGON_INTERACTIVE_TOKEN` | ❌ the logon type is **forced** to `TASK_LOGON_GROUP`; low integrity |
| `UserId` principal (current user SID) + `TASK_LOGON_INTERACTIVE_TOKEN` + `HighestAvailable` | ❌ **still low integrity** (the first two lines above) |
| Boot trigger + **SYSTEM** principal + session 0 host + `CreateProcessAsUser` relay | ❌ a SYSTEM task gets the same low-integrity restricted token |

The raw output from the last one:

```
identity:   account=NT AUTHORITY\SYSTEM session=0 ... integrity=low(0x1000) elevated=yes
privileges: SeTcb=1 SeAssignPrimaryToken=1 SeIncreaseQuota=1
WTSQueryUserToken(session 1) failed: error 5      <- ERROR_ACCESS_DENIED
```

The account is SYSTEM and all three privileges really are enabled, and it is still refused —
a restricted token cannot call `WTSQueryUserToken`.

**They have one thing in common: if the Task Scheduler starts the process, the token is
restricted.** Neither the principal (current user or SYSTEM), the logon type
(`InteractiveToken` or `ServiceAccount`) nor the run level (`HighestAvailable`) changes that.

### What to do for now

1. Run the configurator (one UAC prompt), set up your mapping, click **Save**.
2. Click **Remove** to delete the auto-start task — while it exists, **Start** goes through the
   scheduled task and launches an instance that receives no keys.
3. After each boot, click **Start** once, or simply double-click
   `RemapCopilotKeyService.exe`. That path runs at `高(0x3000)` and works; the only cost is one
   UAC confirmation.

The service can also be started from the command line (same single UAC prompt):

```bat
RemapCopilotKeyService.exe
```

### If you want to keep digging: what has not been ruled out

In rough order of promise, none of these has been tested yet:

1. **Use a real SCM-registered Windows service** (LocalSystem, auto-start). Its token is created
   by the SCM through the service logon and does not go through the Task Scheduler hardening —
   this is the only untried carrier that should yield a full SYSTEM token. The cost is code to
   install / remove the service plus installer changes.
2. **Whether the service's `requireAdministrator` manifest is what triggers the hardening.**
   The comparable NoCopilotKey has **no manifest at all** (`asInvoker`) and its scheduled task is
   reported to work. Removing the manifest from our service is a cheap experiment — but then it
   can only ever run at **medium integrity**, so input going to elevated windows still cannot be
   intercepted.
3. **Whether Group Policy / SRP / AppLocker on this machine affects task tokens.** Reproducing on
   a clean machine would separate "general Windows behaviour" from "a local policy".

### How to tell whether it actually worked

**"Registered successfully" proves nothing** — approaches 1, 3 and 4 above are all silent. Only
this line in `boot.log` matters:

```
运行环境: ... 完整性=高(0x3000) 提权=是
```

`AutoStart.Install` also **reads the task back** and checks `Principal.LogonType`, deleting the
task with a precise error if it is not `InteractiveToken` — but that only proves the task
*definition* is right, **not the token integrity**, as one measurement already proved.

To inspect the definition actually stored by the system:

```
RemapCopilotKey.exe --export-task task.xml
```

## Localisation

Language packs are embedded resources, `src/Configurator/Lang/*.json`:

```json
{
  "code": "en-US",
  "name": "English",
  "strings": { "app.title": "RemapCopilotKey Configurator" }
}
```

To add a language, copy one pack, change `code` / `name`, translate `strings` and rebuild —
**no code changes**. `Configurator.Tests` asserts that every pack has exactly the same key set,
so a missing translation fails the tests.

The service's dialogs are bilingual too and follow the `language` field in the config.

## Testing and verification

```bat
build.cmd test
```

This runs, in order:

| Suite | Assertions | Coverage |
|---|---|---|
| `ConfigContractTests` (C#) | 126 | JSON field names, round-tripping, validation, the key table, language-pack completeness, the bundled JSON layer; also writes a sample config |
| `json_test` (C++) | 68 | The bundled parser: syntax, escapes, UTF-8, surrogate pairs, error positions, depth limits |
| `config_test` (C++) | 164 | Key-name round-trip, config parsing, every action type, fallback behaviour, UTF-8 paths — and it **reads back the config the C# side wrote** and checks it item by item |

The C#-writes → C++-reads loop is where this project is most likely to break (one renamed field
invalidates everything), which is why it is verified across languages.

To dump the config JSON the configurator produces:

```bat
build\out\Configurator\RemapCopilotKey.exe --dump-default-config test.json
```

## Known limitations

- **Auto-start does not work today**: an instance started by the scheduled task runs at low
  integrity (0x1000) and its hook receives no keys. Start it **manually** after each boot
  (one UAC prompt). See [Auto-start: currently unavailable](#auto-start-currently-unavailable).
- **It must run in an interactive session**; it cannot be registered as a Windows service
  (session 0 receives no input from the interactive desktop).
- **Secure desktop is out of reach**: Ctrl+Alt+Del, UAC prompts and the logon screen are not
  visible to a user-mode hook. Covering those would need a keyboard filter driver, which
  requires a Microsoft-signed driver (see [Background research](#background-research)).
- **Hooks are chained**: when several programs install low-level hooks, the most recent one sees
  input first. If something like Remote Desktop grabs it, restarting the service puts you back
  at the head of the chain.
- **Running without administrator rights** means input going to elevated windows cannot be
  intercepted. The program logs a warning at startup.
- **The sequence window is 100 ms.** On a very slow Bluetooth keyboard, if the three keys are
  more than 100 ms apart the sequence is read as the user pressing Win and Shift, and the Copilot
  key does not fire (normal keys are unaffected).
- 64-bit Windows only.

## Troubleshooting

**Start with the two log files** (both in the program folder):

| File | Contents |
|---|---|
| `RemapCopilotKey.boot.log` | Boot trace, **unaffected by `logLevel`**. Start here for "auto-start did nothing" or "the hook receives no keys" |
| `RemapCopilotKeyService.log` | The ordinary log; verbosity follows `logLevel` (default: `error` only) |

| Symptom | What to check |
|---|---|
| Copilot key does nothing | Is `enabled` true? Does the `完整性=` field in `boot.log` say `高(0x3000)`? Set `logLevel` to `debug` and watch the full key stream |
| It behaves as `Win+Shift+…` | The sequence never completed — usually the three keys were more than 100 ms apart (Bluetooth), or another hook took priority |
| Ctrl sticks down | Usually after switching to an elevated window. Confirm the service runs elevated, or restart the service to re-hook |
| Config changes have no effect | Make sure the configurator and the service are in the **same folder**; or press **Save** so the configurator posts the reload message |
| Nothing works in elevated windows | The service must run elevated *and* in the same logon session |
| Auto-start is registered but the Copilot key still does nothing | **Known issue**: the task-started instance runs at low integrity (0x1000) and the hook receives no keys. Check `完整性=` in `boot.log`; click **Remove** to delete the task, then start the service **manually** |
| Auto-start is registered but the service never starts at all | Find `RemapCopilotKey` in Task Scheduler; use `--export-task` to inspect its logon type and run level; or click **Start** once and see whether it errors |
| "Register" reports access denied | Run the configurator as administrator — registering a task requires it |

## Development rules (lessons learned the hard way)

1. **`.cmd` / `.bat` must be pure ASCII with CRLF line endings.** `cmd.exe` parses batch files
   using the OEM code page, so the UTF-8 bytes of Chinese comments pair up with following
   structural characters and swallow `{` or `)`, breaking the script. `.gitattributes` forces
   `eol=crlf`.
2. **`.ps1` files must have a UTF-8 BOM.** Without it Windows PowerShell 5.1 decodes them as
   GBK and the same problem appears — a `Missing closing '}'` from `build.ps1` is this bug.
3. **The linker needs `/MANIFESTUAC:NO`.** Otherwise the bundled `app.manifest` and the UAC
   fragment MSVC generates by default disagree on `level` and `mt.exe` fails with `c1010001`.
4. **Windows headers do not define `VK_A`..`VK_Z`** — the virtual key code of a letter is its
   uppercase ASCII value.
5. **DPI**: WPF is resolution-independent; declaring `dpiAware` / `dpiAwareness=PerMonitorV2` in
   `app.manifest` is enough.
6. **C++ sources are UTF-8 without BOM**, compiled with `/utf-8`.
7. **C# sources are UTF-8 without BOM**, `.xaml` too — but `.ps1` **must** have a BOM (rule 2).

More detail (Chinese): [docs/开发笔记.md](docs/开发笔记.md).

## About "Unknown publisher"

This project **does not sign its binaries**. An unsigned executable's behaviour depends on
whether it requests elevation:

| Executable | Manifest | Double-click |
|---|---|---|
| `RemapCopilotKey.exe` (configurator) | `requireAdministrator` | requests elevation -> the UAC dialog shows "Unknown publisher" |
| `RemapCopilotKeyService.exe` (service) | `requireAdministrator` | same |

**Note that "Unknown publisher" is only a signature line inside the elevation prompt** — it is
not a general "you are running an unsigned program" warning. Programs that never request
elevation never show it; both executables here need administrator rights, so both do.

There are two ways to avoid it — **but one of them no longer works**:

1. ~~Auto-start through the scheduled task~~ — **this no longer holds.** The task really does
   start the service elevated without any UAC prompt, but the instance it starts runs at **low
   integrity** and its hook receives no keys
   (see [Auto-start: currently unavailable](#auto-start-currently-unavailable)).
2. **Buy a code-signing certificate (OV / EV) from a trusted CA** and sign the binaries — this is
   the only option left for now.

> A self-signed certificate only works on machines where you installed it, so it is useless for
> anyone who downloads your build. That is why there is no signing script in the repository.

## Background research

Why this project did **not** use an existing signed keyboard filter driver:

- The only general solution is [Interception](https://github.com/oblitum/Interception), and it
  really is driver-level (it can cover the UAC desktop and the logon screen). But:
  - its last release is **v1.0.1 from 2017**, built with **WDK 7.1**, and only claims testing up
    to Windows 10;
  - the driver is signed with the author's personal GlobalSign certificate (**a cross-signature
    that expired in 2012**), not with a Microsoft WHQL / attestation signature;
  - known issues: if the client crashes, keyboard and mouse stay dead until reboot; installation
    fails on Secure Boot machines.
- The project therefore uses a user-mode hook as its main approach.

---

**If this project helped you, a star at <https://github.com/Anlerway/RemapCopilotKey> is appreciated.**
