// ---------------------------------------------------------------------------
// RemapCopilotKey 服务程序 (RemapCopilotKeyService.exe)
//
// 设计定位：**纯后台静默进程，固定以管理员权限运行**。
//   * 没有托盘图标，没有菜单，没有任何可见窗口（只有一个隐藏的消息窗口）
//   * 用户界面完全由独立的配置程序（RemapCopilotKey.exe）负责
//   * 出错只写日志；只有"钩子装不上"这类致命错误才弹一次对话框
//
// 为什么要管理员权限：低级键盘钩子（WH_KEYBOARD_LL）受 UIPI 限制，
//   非提升进程看不到送往提升窗口的输入。
//
// —— 同一个 exe，两种跑法 ——
//
//  (a) --service    Windows 服务（SCM 以 LocalSystem 启动，开机自启就靠它）
//      跑在 Session 0，装不了钩子。职责只有一个：监督交互式会话，把
//      **自己**以 --run 投递进用户会话（WTSQueryUserToken +
//      CreateProcessAsUser），并保证服务停止时子进程一起收掉。
//      见 ServiceHost.h / ServiceHost.cpp。
//
//  (b) 默认 / --run  用户会话里的钩子进程，真正装 WH_KEYBOARD_LL 的那个。
//      被 (a) 投递时继承的是**用户完整令牌**（高完整性），所以拦得住
//      送往提升窗口的按键；由配置程序手动"启动服务"时同样如此。
//
// 历史教训（不要再往回走）：计划任务 RunLevel=HighestAvailable +
//   LogonType=InteractiveToken 本该给出高完整性，实测给的是**低完整性
//   受限令牌**（提权=是 完整性=低），钩子装得上但一个按键都收不到；
//   "启动时任务 + Session 0 宿主"也试过，`WTSQueryUserToken` 直接
//   ERROR_ACCESS_DENIED。所以自启宿主换成了真正的 SCM 服务 —— 服务令牌
//   是完整 SYSTEM 令牌，不受计划任务那套令牌硬化影响。
// ---------------------------------------------------------------------------

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "Actions.h"
#include "AutoStart.h"
#include "Config.h"
#include "CopilotHook.h"
#include "Keys.h"
#include "Log.h"
#include "ServiceHost.h"

using namespace rck;

namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// 服务窗口的类名。配置程序靠它找到本进程并发送控制消息。
// 它同时也是"是否已有实例在运行"的唯一判据（见 wWinMain 里的单实例段）。
constexpr wchar_t kWindowClassName[] = L"RemapCopilotKeyService.MessageWindow";

constexpr UINT kConfigPollTimerId  = 2;
constexpr UINT kConfigPollInterval = 1000;   // 毫秒

// 控制消息（与 C# 侧 ServiceControl.cs 保持一致）
constexpr UINT WM_RCK_RELOAD = WM_APP + 4;
constexpr UINT WM_RCK_QUIT   = WM_APP + 5;

// ---------------------------------------------------------------------------
// 仅剩的对话框文本
//
// 服务程序静默运行，正常情况下不会出现任何界面；这个字符串只用于
// "钩子安装失败" 这一种情况。开机自启相关的提示已经全部搬到配置程序里
// （注册见 src/Configurator/ServiceInstaller.cs，投递见 src/Service/src/ServiceHost.cpp）。
// ---------------------------------------------------------------------------

enum StringId : int {
    S_TITLE = 0,
    S_HOOK_FAILED,
    S_COUNT,
};

struct LocalizedString {
    const wchar_t* zh;
    const wchar_t* en;
};

const LocalizedString kStrings[S_COUNT] = {
    /* S_TITLE */       { L"RemapCopilotKey", L"RemapCopilotKey" },
    /* S_HOOK_FAILED */ { L"无法安装键盘钩子，详见 RemapCopilotKeyService.log。",
                          L"Could not install the keyboard hook. See RemapCopilotKeyService.log." },
};

bool g_chinese = true;

const wchar_t* T(StringId id) {
    const LocalizedString& s = kStrings[id];
    return g_chinese ? s.zh : s.en;
}

// ---------------------------------------------------------------------------
// 应用状态
// ---------------------------------------------------------------------------

struct AppState {
    Config        config;
    ConfigWatcher watcher;
    CopilotHook   hook;
    ActionExecutor executor;

    std::wstring configPath;
    std::wstring logPath;
    std::wstring moduleDir;
    std::wstring exePath;

    HWND      hwnd     = nullptr;
    HINSTANCE instance = nullptr;

    bool configValid = true;
    bool quiet       = false;   // 命令行模式：不弹对话框
};

AppState g_app;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void ApplyConfig() {
    g_chinese = EqualsIgnoreCase(g_app.config.language, L"zh-CN") ||
                g_app.config.language.empty();

    LogSetLevel(LogLevelFromName(g_app.config.logLevel));
    g_app.hook.SetEnabled(g_app.config.enabled && g_app.configValid);
}

void LoadConfig(HWND hwnd, bool showErrors) {
    std::wstring error;
    Config loaded;

    const bool ok = ConfigLoad(g_app.configPath, loaded, error);

    if (!ok) {
        if (GetFileAttributesW(g_app.configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring saveError;
            if (ConfigSave(g_app.configPath, DefaultConfig(), saveError)) {
                RCK_LOG_INFO(L"未找到配置文件，已生成默认配置: %s", g_app.configPath.c_str());
                g_app.config = DefaultConfig();
                g_app.configValid = true;
                g_app.watcher.Watch(g_app.configPath);
                ApplyConfig();
                return;
            }
            RCK_LOG_ERROR(L"生成默认配置失败: %s", saveError.c_str());
        }

        RCK_LOG_ERROR(L"读取配置失败: %s", error.c_str());
        g_app.configValid = false;
        g_app.config = DefaultConfig();

        if (showErrors && !g_app.quiet) {
            std::wstring body = L"配置文件有问题，已改用默认设置运行：\r\n\r\n" + error;
            MessageBoxW(hwnd, body.c_str(), T(S_TITLE), MB_OK | MB_ICONWARNING);
        }
        ApplyConfig();
        return;
    }

    g_app.configValid = true;
    g_app.config = loaded;

    if (!error.empty()) {
        RCK_LOG_WARN(L"配置警告: %s", error.c_str());
    } else {
        RCK_LOG_INFO(L"配置已加载: 动作=%s, 启用=%d",
                     ActionTypeName(g_app.config.action.type),
                     g_app.config.enabled ? 1 : 0);
    }

    ApplyConfig();
}

void ReloadConfigFromDisk() {
    RCK_LOG_INFO(L"检测到配置文件变化，重新加载");
    LoadConfig(g_app.hwnd, /*showErrors=*/false);
}

// ---------------------------------------------------------------------------
// 窗口过程（隐藏消息窗口，不创建任何可见界面）
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_RCK_TRIGGER_DOWN:
        g_app.executor.HandlePress(g_app.config.action);
        return 0;

    case WM_RCK_TRIGGER_UP:
        g_app.executor.HandleRelease(g_app.config.action);
        return 0;

    case WM_RCK_SEND_INPUT: {
        const WORD vkey     = static_cast<WORD>(wParam);
        const bool keyUp    = (lParam & 1) != 0;
        const bool extended = (lParam & 2) != 0;
        SendKeyInput(vkey, keyUp, extended);
        return 0;
    }

    case WM_RCK_RELOAD:
        ReloadConfigFromDisk();
        return 0;

    case WM_RCK_QUIT:
        DestroyWindow(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == CopilotHook::kChordTimerId) {
            g_app.hook.OnChordTimeout();
        } else if (wParam == kConfigPollTimerId) {
            if (g_app.config.reloadOnChange && g_app.watcher.PollChanged()) {
                ReloadConfigFromDisk();
            }
        }
        return 0;

    case WM_ENDSESSION:
        g_app.executor.ForceReleaseAll();
        return 0;

    case WM_DESTROY:
        g_app.executor.ForceReleaseAll();
        g_app.hook.Uninstall();
        KillTimer(hwnd, kConfigPollTimerId);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 控制通道
// ---------------------------------------------------------------------------

HWND FindServiceWindow() {
    return FindWindowW(kWindowClassName, nullptr);
}

// 放行控制消息，避免因为 UIPI 导致权限不同的进程无法控制提权后的服务。
// （配置程序与服务程序同级运行，但用户也可能用普通权限的进程发这些消息。）
void AllowControlMessages(HWND hwnd) {
    const UINT messages[] = { WM_RCK_RELOAD, WM_RCK_QUIT };
    for (UINT msg : messages) {
        if (!ChangeWindowMessageFilterEx(hwnd, msg, MSGFLT_ALLOW, nullptr)) {
            RCK_LOG_DEBUG(L"ChangeWindowMessageFilterEx(0x%04X) 失败：%lu", msg, GetLastError());
        }
    }
}

// ---------------------------------------------------------------------------
// 命令行
//
// 只保留"运行钩子"和"停止钩子"两条，外加 --service（SCM 服务宿主）。
// 服务的注册、启动、删除**不在这里**，而在配置程序里 ——
// 见 src/Configurator/ServiceInstaller.cs，以及该文件头部对原因的说明。
// ---------------------------------------------------------------------------

struct CommandLineOptions {
    bool run     = true;
    bool stop    = false;
    bool quiet   = false;
    bool help    = false;
    bool service = false;   // --service：以 SCM 服务宿主身份运行（见 ServiceHost.h）

    std::wstring configPath;
    std::string  logLevel;
};

CommandLineOptions ParseCommandLine() {
    CommandLineOptions options;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return options;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        auto nextValue = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (EqualsIgnoreCase(arg, L"--stop"))            { options.stop = true; options.run = false; }
        else if (EqualsIgnoreCase(arg, L"--quiet"))      { options.quiet = true; }
        else if (EqualsIgnoreCase(arg, L"--run"))        { /* 默认行为 */ }
        else if (EqualsIgnoreCase(arg, L"--service"))    { options.service = true; options.run = false; }
        else if (EqualsIgnoreCase(arg, L"--config"))     { nextValue(options.configPath); }
        else if (EqualsIgnoreCase(arg, L"--loglevel")) {
            std::wstring value;
            if (nextValue(value)) options.logLevel = WideToUtf8(value);
        }
        else if (EqualsIgnoreCase(arg, L"--help") ||
                 EqualsIgnoreCase(arg, L"-h") ||
                 EqualsIgnoreCase(arg, L"/?"))           { options.help = true; options.run = false; }
    }

    LocalFree(argv);
    return options;
}

// ---------------------------------------------------------------------------
// 命令行动作
// ---------------------------------------------------------------------------

// 请求正在运行的实例退出。返回 0 成功，2 表示没有实例在跑。
int HandleStop() {
    const HWND hwnd = FindServiceWindow();
    if (!hwnd) {
        RCK_LOG_INFO(L"没有正在运行的服务实例");
        if (!g_app.quiet) {
            MessageBoxW(nullptr, L"服务未在运行。", T(S_TITLE), MB_OK | MB_ICONINFORMATION);
        }
        return 2;
    }

    if (!PostMessageW(hwnd, WM_RCK_QUIT, 0, 0)) {
        RCK_LOG_ERROR(L"发送停止消息失败（错误码 %lu）", GetLastError());
        return 1;
    }
    RCK_LOG_INFO(L"已请求运行中的实例退出");
    return 0;
}

int HandleHelp() {
    const wchar_t* text =
        L"RemapCopilotKeyService\n\n"
        L"  --run      以钩子进程运行（默认）\n"
        L"  --service  以 Windows 服务宿主运行（只能由 SCM 启动）\n"
        L"  --stop     请运行中的钩子进程退出\n"
        L"  --config <路径>  --loglevel <级别>  --quiet  --help\n\n"
        L"服务的注册与删除在配置程序 RemapCopilotKey.exe 里，不在这里。\n";
    MessageBoxW(nullptr, text, T(S_TITLE), MB_OK | MB_ICONINFORMATION);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // 解析 .lnk 快捷方式（IShellLink）需要 COM。失败也不影响主要功能。
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_app.instance  = hInstance;
    g_app.moduleDir = GetModuleDirectory();
    g_app.logPath   = GetDefaultLogPath();

    wchar_t exeBuffer[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, exeBuffer, static_cast<DWORD>(std::size(exeBuffer)));
    g_app.exePath = exeBuffer;

    const CommandLineOptions options = ParseCommandLine();
    g_app.quiet = options.quiet;

    g_app.configPath = options.configPath.empty() ? GetDefaultConfigPath() : options.configPath;

    LogSetFile(g_app.logPath);
    // 默认只记错误：正常使用时不该产生大量日志。
    // 配置里的 logLevel 会覆盖它（见 ApplyConfig / LoadConfig）。
    LogSetLevel(options.logLevel.empty() ? LogLevel::Error : LogLevelFromName(options.logLevel));

    RCK_LOG_INFO(L"==== RemapCopilotKeyService 启动 (pid=%lu) ====", GetCurrentProcessId());
    RCK_LOG_INFO(L"程序目录: %s", g_app.moduleDir.c_str());
    RCK_LOG_INFO(L"配置文件: %s", g_app.configPath.c_str());
    RCK_LOG_INFO(L"当前权限: %s", IsRunningElevated() ? L"管理员" : L"普通用户");

    // 启动追踪：**不受 logLevel 影响**，永远写文件。
    // 排查"开机自启没生效"时这是唯一可靠的线索 —— Session 0 宿主连窗口都没有，
    // 而用户机器上的 logLevel 多半还是默认的 error，普通日志一条都不会记。
    BootTraceSetFile(g_app.moduleDir + L"\\RemapCopilotKey.boot.log");
    RCK_BOOT(L"进程启动 pid=%lu 模式=%s",
             GetCurrentProcessId(),
             options.service ? L"service"
                             : (options.stop ? L"stop" : (options.help ? L"help" : L"run")));

    if (options.help)      return HandleHelp();
    if (options.stop)      return HandleStop();

    // --- Windows 服务宿主：只负责把钩子投递进用户会话，自己不装钩子 ---
    if (options.service) {
        const int result = RunAsServiceHost(g_app.quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return result;
    }

    // --- 正常运行模式（后台静默） ---

    // --- 读取配置 ---
    {
        std::wstring error;
        Config loaded;
        if (ConfigLoad(g_app.configPath, loaded, error)) {
            g_app.config = loaded;
            g_app.configValid = true;
        } else {
            g_app.config = DefaultConfig();
            g_app.configValid = false;

            if (GetFileAttributesW(g_app.configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring saveError;
                if (ConfigSave(g_app.configPath, g_app.config, saveError)) {
                    g_app.configValid = true;
                    RCK_LOG_INFO(L"未找到配置文件，已生成默认配置: %s", g_app.configPath.c_str());
                } else {
                    RCK_LOG_ERROR(L"生成默认配置失败: %s", saveError.c_str());
                }
            }
            if (!g_app.configValid) {
                RCK_LOG_WARN(L"读取配置失败，先按默认设置运行: %s", error.c_str());
            }
        }
        ApplyConfig();
    }

    // 放在 ApplyConfig 之后：这样这一行会跟着用户的 logLevel 走。
    // 排查"钩子装上了但收不到按键"时，第一步就是把 logLevel 调成 info 或 debug，
    // 然后对比这一行在"手动启动"与"开机自动启动"下有什么不同 —— 两者的
    // 会话/窗口站/桌面/完整性都应该是同一套值。
    RCK_LOG_INFO(L"运行环境: %s", DescribeExecutionEnvironment().c_str());

    // 同一行同时写进不受 logLevel 影响的启动追踪。
    // 这一行是"自启到底成没成"的唯一判据（服务宿主那一侧记的是宿主自己的环境），
    // 而默认配置的 logLevel 是 error —— 只写普通日志等于没写。
    RCK_BOOT(L"运行环境: %s", DescribeExecutionEnvironment().c_str());

    if (!IsRunningElevated()) {
        RCK_LOG_WARN(L"当前不是以管理员权限运行，送往管理员窗口的按键将无法拦截。");
    }

    // --- 单实例 ---
    //
    // 判据只有一个：**隐藏消息窗口存不存在**。窗口在 = 真有一个实例在跑。
    //
    // 这里踩过一个很难查的坑：早期还用一个命名互斥体（Global\...）做判定，
    // 只要 CreateMutexW 报 ERROR_ALREADY_EXISTS 就 return 0。问题是"互斥体存在"
    // ≠ "另一个实例在正常工作"，于是一旦互斥体被留下，新进程就会**静默退出、
    // 连钩子都不装** —— 表现为"进程启动了、日志也写了，但按键完全没反应"，
    // 而且手动重启服务也一样失败（同一个误判再来一次）。
    //
    // 现在把互斥体**整个删掉**：窗口探测已经足够准确（隐藏窗口没有 UI、
    // 不可能被误认为用户窗口），再加一层只会制造误判。
    // 重复启动最坏的结果是多出一个实例，钩子多挂一份，按键行为不变；
    // 而误判的后果是"服务完全失效"，代价高得多。
    if (FindServiceWindow() != nullptr) {
        RCK_LOG_INFO(L"已有服务实例在运行（找到消息窗口），本实例退出");
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&wc)) {
        RCK_LOG_ERROR(L"RegisterClassExW 失败（错误码 %lu）", GetLastError());
        return 1;
    }

    g_app.hwnd = CreateWindowExW(0, kWindowClassName, L"RemapCopilotKeyService", 0,
                                 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_app.hwnd) {
        RCK_LOG_ERROR(L"CreateWindowExW 失败（错误码 %lu）", GetLastError());
        return 1;
    }

    AllowControlMessages(g_app.hwnd);
    LoadConfig(g_app.hwnd, /*showErrors=*/!g_app.quiet);

    if (!g_app.hook.Install(g_app.hwnd, [](TriggerEvent event) {
            PostMessageW(g_app.hwnd,
                         event == TriggerEvent::Press ? WM_RCK_TRIGGER_DOWN : WM_RCK_TRIGGER_UP,
                         0, 0);
        })) {
        if (!g_app.quiet) {
            MessageBoxW(nullptr, T(S_HOOK_FAILED), T(S_TITLE), MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    g_app.watcher.Watch(g_app.configPath);
    SetTimer(g_app.hwnd, kConfigPollTimerId, kConfigPollInterval, nullptr);

    RCK_LOG_INFO(L"服务已就绪（后台静默运行）");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RCK_LOG_INFO(L"==== RemapCopilotKeyService 退出 ====");
    if (SUCCEEDED(comResult)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
