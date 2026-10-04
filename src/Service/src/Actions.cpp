// ---------------------------------------------------------------------------
// RemapCopilotKey - 动作执行器实现
// ---------------------------------------------------------------------------

#include "Actions.h"

#include "Keys.h"
#include "Log.h"

#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

namespace rck {

namespace {

// Windows 里方向键 / 小键盘键共用一套虚拟键码，MapVirtualKeyExW 无法区分，
// 这里按"是否扩展键"补上 0xE000 前缀，注入出来的扫描码才正确。
bool IsNumpadSharedKey(WORD vkey) {
    switch (vkey) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_LEFT:  case VK_UP:   case VK_RIGHT: case VK_DOWN:
    case VK_INSERT: case VK_DELETE: case VK_NUMLOCK: case VK_RSHIFT:
        return true;
    default:
        return false;
    }
}

} // namespace

// 这些虚拟键码在按下时必须带扩展位，否则会被当成小键盘上的另一个键
bool IsAlwaysExtendedKey(WORD vkey) {
    switch (vkey) {
    case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: case VK_RWIN: case VK_APPS:
    case VK_SNAPSHOT:
        return true;
    default:
        return false;
    }
}

UINT VKeyToScanCode(WORD vkey, bool extended) {
    const UINT base = MapVirtualKeyW(vkey, MAPVK_VK_TO_VSC_EX);

    if (IsNumpadSharedKey(vkey)) {
        // 扩展键（真正的方向键等）需要 0xE000 前缀；否则是小键盘上的版本
        return extended ? (base | 0xE000u) : base;
    }
    if (extended) {
        switch (vkey) {
        case VK_RETURN:   return base | 0xE000u;   // 小键盘回车
        case VK_SNAPSHOT: return 0xE037u;          // SysRq
        case VK_DIVIDE:   return base | 0xE000u;
        case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_RCONTROL: case VK_RMENU:
            return base | 0xE000u;
        default:
            break;
        }
    }
    if (vkey == VK_PAUSE) return 0x45;
    return base;
}

bool SendKeyInput(WORD vkey, bool keyUp, bool extended) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkey;

    // 调用方没指定时，按虚拟键码自动判断常用扩展键
    if (IsAlwaysExtendedKey(vkey)) extended = true;

    const UINT scan = VKeyToScanCode(vkey, extended);
    input.ki.wScan = static_cast<WORD>(scan & 0xFF);
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    if (scan >= 0xE000u || extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;

    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        RCK_LOG_WARN(L"SendInput 失败（vkey=0x%02X, keyUp=%d, err=%lu）",
                     vkey, keyUp ? 1 : 0, GetLastError());
        return false;
    }
    return true;
}

bool SendChordInput(const std::vector<WORD>& keys) {
    if (keys.empty()) return false;

    bool ok = true;
    for (WORD vk : keys) {
        ok = SendKeyInput(vk, false) && ok;
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        ok = SendKeyInput(*it, true) && ok;
    }
    return ok;
}

bool IsTokenElevated(HANDLE token) {
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) return false;
    return elevation.TokenIsElevated != 0;
}

bool IsProcessElevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    const bool elevated = IsTokenElevated(token);
    CloseHandle(token);
    return elevated;
}

// 取一个非提权的用户令牌：优先用 shell 进程的令牌；
// 如果 shell 本身处于提权状态，就改用它关联的受限（filtered）令牌。
HANDLE AcquireUserToken() {
    const HWND shellWindow = GetShellWindow();
    if (!shellWindow) {
        RCK_LOG_WARN(L"取不到 shell 窗口，无法降权");
        return nullptr;
    }

    DWORD shellPid = 0;
    GetWindowThreadProcessId(shellWindow, &shellPid);
    if (shellPid == 0) {
        RCK_LOG_WARN(L"取不到 shell 进程号，无法降权");
        return nullptr;
    }

    HANDLE shellProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid);
    if (!shellProcess) {
        RCK_LOG_WARN(L"打不开 shell 进程（错误码 %lu）", GetLastError());
        return nullptr;
    }

    HANDLE shellToken = nullptr;
    const BOOL opened = OpenProcessToken(shellProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &shellToken);
    CloseHandle(shellProcess);
    if (!opened) {
        RCK_LOG_WARN(L"打不开 shell 令牌（错误码 %lu）", GetLastError());
        return nullptr;
    }

    const bool shellElevated = IsTokenElevated(shellToken);
    RCK_LOG_INFO(L"shell 进程 pid=%lu，其令牌已提权=%d", shellPid, shellElevated ? 1 : 0);

    HANDLE source = shellToken;
    TOKEN_LINKED_TOKEN linked{};
    DWORD size = 0;
    if (shellElevated &&
        GetTokenInformation(shellToken, TokenLinkedToken, &linked, sizeof(linked), &size) &&
        linked.LinkedToken) {
        source = linked.LinkedToken;
        RCK_LOG_INFO(L"shell 令牌是提权的，改用其受限令牌");
    }

    HANDLE primary = nullptr;
    const BOOL duplicated = DuplicateTokenEx(source, TOKEN_ALL_ACCESS, nullptr,
                                             SecurityImpersonation, TokenPrimary, &primary);

    if (linked.LinkedToken) CloseHandle(linked.LinkedToken);
    CloseHandle(shellToken);

    if (!duplicated) {
        RCK_LOG_WARN(L"复制令牌失败（错误码 %lu）", GetLastError());
        return nullptr;
    }
    return primary;
}

// 以普通用户权限创建进程。本服务是管理员，直接 ShellExecute 会让被打开的程序
// 继承管理员令牌，浏览器会报「现有实例正在以提升的权限运行」，所以要用
// 登录用户自己的令牌来起。
//
// 这里**只有一条路径**：CreateProcessWithTokenW。
//
// 曾经为了"万一失败"加过一条 CreateProcessAsUserW 的回落，后来全部删掉了 ——
// 那个失败根本不是 API 选得不对，而是**服务拿到的令牌本身是低完整性**：
// 那时计划任务的主体写成了 GroupId=Users，被系统改写成 TASK_LOGON_GROUP
// （"以组身份运行"），于是既没有真实用户令牌、也拿不到
// SeAssignPrimaryTokenPrivilege，两条路径都会失败。
//
// 修好主体（UserId + InteractiveToken）之后，服务拿到的是**当前登录用户的
// 高完整性令牌**，CreateProcessWithTokenW 直接成功。回落路径从来没有真正
// 解决问题，只是把"令牌不对"这个真实原因掩盖成"某条 API 失败"。
bool LaunchAsUser(const std::wstring& file, const std::wstring& parameters) {
    HANDLE userToken = AcquireUserToken();
    if (!userToken) return false;

    std::wstring commandLine = L"\"" + file + L"\"";
    if (!parameters.empty()) commandLine += L" " + parameters;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL started = CreateProcessWithTokenW(
        userToken, 0, nullptr, commandLine.data(),
        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);

    if (!started) {
        RCK_LOG_WARN(L"CreateProcessWithTokenW 失败（错误码 %lu）", GetLastError());
    }

    CloseHandle(userToken);
    if (!started) return false;

    // 记录新进程实际拿到的权限，便于确诊浏览器是否真的降权成功
    RCK_LOG_INFO(L"新进程 pid=%lu 权限=%s", pi.dwProcessId,
                 IsProcessElevated(pi.hProcess) ? L"管理员" : L"普通");

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool ShellOpen(const std::wstring& target, const std::wstring& arguments, bool runAsAdmin) {
    if (target.empty()) {
        RCK_LOG_WARN(L"ShellOpen 的目标为空，忽略。");
        return false;
    }

    // 只有用户明确勾选「以管理员权限运行」才提权；其余一律以登录用户身份启动。
    // 这里是唯一的两种去向，不存在"先按普通权限、失败就提权"的回落，
    // 否则就等于在用户没勾选的情况下偷偷给了管理员权限。
    if (!runAsAdmin) {
        // URL 没有自己的可执行文件，借 rundll32 交给默认浏览器
        const bool isUrl = target.find(L"://") != std::wstring::npos;
        const bool opened = isUrl
            ? LaunchAsUser(L"rundll32.exe", L"url.dll,FileProtocolHandler \"" + target + L"\"")
            : LaunchAsUser(target, arguments);

        if (opened) {
            RCK_LOG_INFO(L"已打开（普通）: %s", target.c_str());
        } else {
            RCK_LOG_WARN(L"以登录用户身份打开失败: %s", target.c_str());
        }
        return opened;
    }

    const HINSTANCE result = ShellExecuteW(
        nullptr, L"runas", target.c_str(),
        arguments.empty() ? nullptr : arguments.c_str(),
        nullptr, SW_SHOWNORMAL);

    // ShellExecute 的返回值 <= 32 表示失败
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        RCK_LOG_WARN(L"以管理员身份打开失败（返回值 %lld）: %s",
                     static_cast<long long>(reinterpret_cast<INT_PTR>(result)),
                     target.c_str());
        return false;
    }
    RCK_LOG_INFO(L"已打开（管理员）: %s", target.c_str());
    return true;
}

void ResolveSystemAction(SystemActionId id, std::wstring& target, std::wstring& arguments,
                         std::vector<WORD>& chord) {
    target.clear();
    arguments.clear();
    chord.clear();

    switch (id) {
    case SystemActionId::Explorer:
        target = L"explorer.exe";
        break;
    case SystemActionId::TaskManager:
        target = L"taskmgr.exe";
        break;
    case SystemActionId::Cmd:
        target = L"cmd.exe";
        break;
    case SystemActionId::PowerShell:
        target = L"powershell.exe";
        break;
    case SystemActionId::Screenshot:
        chord = { VK_LWIN, VK_LSHIFT, L'S' };      // Win+Shift+S
        break;
    case SystemActionId::SnippingTool:
        target = L"ms-screenclip:";                // 直接唤起截图工具浮层
        break;
    case SystemActionId::Settings:
        target = L"ms-settings:";
        break;
    case SystemActionId::ControlPanel:
        target = L"control.exe";
        break;
    case SystemActionId::Calculator:
        target = L"calc.exe";
        break;
    case SystemActionId::Notepad:
        target = L"notepad.exe";
        break;
    case SystemActionId::TaskView:
        chord = { VK_LWIN, VK_TAB };
        break;
    case SystemActionId::ClipboardHistory:
        chord = { VK_LWIN, L'V' };
        break;
    case SystemActionId::EmojiPanel:
        chord = { VK_LWIN, VK_OEM_PERIOD };        // Win+.
        break;
    case SystemActionId::LockScreen:
        chord = { VK_LWIN, L'L' };
        break;
    }
}

// ---------------------------------------------------------------------------
// 进程 / 窗口查找
//
// 用于"如果程序/功能已经打开，再次按下就切换或关闭它"这类需求。
// 匹配方式是**可执行文件名**（不区分大小写），这样配置里写全路径
// （C:\Windows\System32\taskmgr.exe）或只写名字（taskmgr.exe）都能匹配上。
// ---------------------------------------------------------------------------

namespace {

void ToLowerInPlace(std::wstring& text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
}

// 从路径里取出可执行文件名（去掉目录与可能的引号）
std::wstring FileNameOf(const std::wstring& pathOrName) {
    std::wstring name = pathOrName;

    const size_t pos = name.find_last_of(L"\\/");
    if (pos != std::wstring::npos) name = name.substr(pos + 1);

    while (!name.empty() && (name.front() == L'"' || name.front() == L' ')) name.erase(name.begin());
    while (!name.empty() && (name.back() == L'"' || name.back() == L' ')) name.pop_back();
    return name;
}

struct WindowSearchContext {
    DWORD     pid = 0;
    HWND      found = nullptr;
    long long bestScore = -1;
};

// 桌面（Progman/WorkerW）与任务栏（Shell_TrayWnd）也是"可见的无主窗口"，
// 而且它们都属于 explorer.exe。如果不过滤，找资源管理器主窗口时会命中桌面，
// 对它最小化不会产生任何可见效果。
bool IsShellWindowClass(HWND hwnd) {
    wchar_t className[64] = {};
    if (GetClassNameW(hwnd, className, 64) == 0) return false;
    return wcscmp(className, L"Progman") == 0
        || wcscmp(className, L"WorkerW") == 0
        || wcscmp(className, L"Shell_TrayWnd") == 0
        || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0
        || wcscmp(className, L"Button") == 0;
}

// 打分挑"最像主窗口"的那个：无主窗口 > 有标题 > 面积大。
// 只取第一个可见窗口的做法会命中各种辅助窗口，不可靠。
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (hwnd == GetShellWindow()) return TRUE;
    if (IsShellWindowClass(hwnd)) return TRUE;

    // 跳过被 DWM 遮蔽的窗口：UWP 与现代应用会留下 IsWindowVisible 为真、
    // 但实际不显示的窗口，选中它会让切换看起来没有反应。
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    // 跳过工具窗
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    const bool owned = GetWindow(hwnd, GW_OWNER) != nullptr;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const long long area = static_cast<long long>(rect.right - rect.left) *
                           static_cast<long long>(rect.bottom - rect.top);

    long long score = area;
    if (!owned) score += 1000000000000LL;
    if (GetWindowTextLengthW(hwnd) > 0) score += 500000000000LL;

    if (score > ctx->bestScore) {
        ctx->bestScore = score;
        ctx->found = hwnd;
    }
    return TRUE;
}

// SetForegroundWindow 受前台锁限制，必要时附加输入线程绕过
void ForceForeground(HWND hwnd) {
    if (SetForegroundWindow(hwnd)) return;

    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();

    if (foregroundThread != 0 && foregroundThread != currentThread &&
        AttachThreadInput(foregroundThread, currentThread, TRUE)) {
        SetForegroundWindow(hwnd);
        AttachThreadInput(foregroundThread, currentThread, FALSE);
    }
}

// .lnk 快捷方式本身不是可执行文件名，先解析出真实目标再拿去匹配进程
std::wstring ResolveShortcutTarget(const std::wstring& path) {
    if (path.size() < 4) return std::wstring();

    std::wstring extension = path.substr(path.size() - 4);
    ToLowerInPlace(extension);
    if (extension != L".lnk") return std::wstring();

    std::wstring target;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&link)))) {
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
            if (SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
                wchar_t buffer[MAX_PATH * 2] = {};
                if (SUCCEEDED(link->GetPath(buffer, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) {
                    target = buffer;
                }
            }
            file->Release();
        }
        link->Release();
    }
    return target;
}

} // namespace

HWND FindRunningAppWindow(const std::wstring& exePathOrName) {
    // 快捷方式先解析成真实目标，否则永远匹配不到进程
    std::wstring source = ResolveShortcutTarget(exePathOrName);
    if (source.empty()) source = exePathOrName;

    std::wstring wanted = FileNameOf(source);
    if (wanted.empty()) return nullptr;

    const std::wstring wantedLower = [&] {
        std::wstring lower = wanted;
        ToLowerInPlace(lower);
        return lower;
    }();

    DWORD pid = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                std::wstring name = entry.szExeFile;
                ToLowerInPlace(name);
                if (name == wantedLower) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (pid == 0) {
        RCK_LOG_DEBUG(L"没有找到正在运行的 %s", wanted.c_str());
        return nullptr;
    }

    WindowSearchContext ctx;
    ctx.pid = pid;
    EnumWindows(&EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found) {
        RCK_LOG_WARN(L"找到 %s 的进程（pid=%lu）但没有可用的顶层窗口"
                     L"（可能窗口还没创建、或只有被过滤掉的外壳/工具窗）",
                     wanted.c_str(), pid);
    } else {
        wchar_t title[128] = {};
        GetWindowTextW(ctx.found, title, 128);
        RCK_LOG_DEBUG(L"命中 %s 的窗口：hwnd=0x%p 标题=\"%s\"",
                      wanted.c_str(), ctx.found, title);
    }
    return ctx.found;
}

void ToggleAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    // 用 GetWindowPlacement 判断状态比只看 IsIconic 更准确
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    const BOOL gotPlacement = GetWindowPlacement(hwnd, &placement);
    const bool minimized = IsIconic(hwnd) != FALSE ||
                           (gotPlacement && placement.showCmd == SW_SHOWMINIMIZED);

    RCK_LOG_INFO(L"窗口状态：minimized=%d showCmd=%u 当前前台=%d",
                 minimized ? 1 : 0,
                 gotPlacement ? placement.showCmd : 0u,
                 GetForegroundWindow() == hwnd ? 1 : 0);

    if (minimized) {
        // 还原：WM_SYSCOMMAND 走目标窗口自己的消息循环，跨进程最可靠；
        // ShowWindow 作补充；随后尝试激活。
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        ShowWindow(hwnd, SW_RESTORE);
        ForceForeground(hwnd);
        RCK_LOG_INFO(L"切换结果：已还原");
        return;
    }

    // 最小化：两种方式**一起用**，缺一不可。
    //   ShowWindow(SW_MINIMIZE)                  直接生效，不依赖目标窗口怎么处理消息
    //   PostMessage(WM_SYSCOMMAND, SC_MINIMIZE)  走目标窗口自己的消息循环，作为补充
    // 只发消息是不够的：PostMessage 返回非零只代表"消息进了队列"，
    // 目标窗口如果不处理 SC_MINIMIZE 就什么都不会发生 ——


    // 也**不要**先抢前台再最小化，那会让窗口闪一下再消失，看起来像没生效。
    ShowWindow(hwnd, SW_MINIMIZE);
    PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);

    RCK_LOG_INFO(L"切换结果：已最小化（iconic=%d）", IsIconic(hwnd) ? 1 : 0);
}

bool CloseAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
}

// ---------------------------------------------------------------------------
// ActionExecutor
// ---------------------------------------------------------------------------

void ActionExecutor::HandlePress(const Action& action) {
    switch (action.type) {
    case ActionType::SendKey:
        // 跟随触发键按住：按下时注入 keydown，松开时注入 keyup
        if (SendKeyInput(action.vkey, false)) {
            m_keyDown = true;
            m_key = action.vkey;
        }
        RCK_LOG_INFO(L"动作: 按住 %s", VKeyName(action.vkey).c_str());
        break;

    case ActionType::SendChord:
        SendChordInput(action.chord);
        {
            std::wstring names;
            for (WORD vk : action.chord) {
                if (!names.empty()) names += L"+";
                names += VKeyName(vk);
            }
            RCK_LOG_INFO(L"动作: 组合键 %s", names.c_str());
        }
        break;

    case ActionType::OpenApp:
        // 开关打开且程序已经在跑：改为最小化 / 还原它的窗口，不再启动第二个实例
        if (action.toggleWindow) {
            const HWND existing = FindRunningAppWindow(action.path);
            if (existing) {
                RCK_LOG_INFO(L"动作: %s 已在运行，切换窗口", action.path.c_str());
                ToggleAppWindow(existing);
                break;
            }
            RCK_LOG_INFO(L"动作: %s 没有找到可切换的窗口，改为直接打开", action.path.c_str());
        }
        RCK_LOG_INFO(L"动作: 打开应用 %s（权限=%s，已在运行时切换窗口=%s）",
                     action.path.c_str(),
                     action.runAsAdmin ? L"管理员" : L"普通",
                     action.toggleWindow ? L"开" : L"关");
        ShellOpen(action.path, action.arguments, action.runAsAdmin);
        break;

    case ActionType::OpenUrl:
        RCK_LOG_INFO(L"动作: 打开网页 %s", action.url.c_str());
        ShellOpen(action.url, L"");
        break;

    case ActionType::SystemAction: {
        std::wstring target, arguments;
        std::vector<WORD> chord;
        ResolveSystemAction(action.systemAction, target, arguments, chord);

        if (!chord.empty()) {
            SendChordInput(chord);
            RCK_LOG_INFO(L"动作: 实用功能 %s（组合键）", SystemActionName(action.systemAction));
            break;
        }

        // 该功能已经打开就再次按下关闭它（能用则用，不能用就正常启动）
        const HWND existing = FindRunningAppWindow(target);
        if (existing) {
            RCK_LOG_INFO(L"动作: 关闭已打开的 %s", target.c_str());
            CloseAppWindow(existing);
            break;
        }

        ShellOpen(target, arguments);
        RCK_LOG_INFO(L"动作: 实用功能 %s -> %s",
                     SystemActionName(action.systemAction), target.c_str());
        break;
    }
    }
}

void ActionExecutor::HandleRelease(const Action& action) {
    if (action.type == ActionType::SendKey && m_keyDown) {
        SendKeyInput(m_key, true);
        RCK_LOG_INFO(L"动作: 松开 %s", VKeyName(m_key).c_str());
        m_keyDown = false;
        m_key = 0;
    }
}

void ActionExecutor::ForceReleaseAll() {
    if (!m_keyDown) return;
    RCK_LOG_INFO(L"强制释放仍然按下的 %s（避免按键卡住）", VKeyName(m_key).c_str());
    SendKeyInput(m_key, true);
    m_keyDown = false;
    m_key = 0;
}

} // namespace rck
