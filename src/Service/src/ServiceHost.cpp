// ---------------------------------------------------------------------------
// RemapCopilotKey - SCM 服务宿主实现
//
// 线程模型：
//   * 主线程跑 StartServiceCtrlDispatcherW → SCM 回调 ServiceMain；
//   * ServiceMain 里先报 SERVICE_RUNNING，然后在**同一个线程**上跑监督循环
//     （停止请求由 SCM 在另一个线程上调 HandlerEx，置 g_stopEvent 即可）；
//   * HandlerEx 只做状态上报与置事件，不做任何阻塞操作。
// ---------------------------------------------------------------------------

#include "ServiceHost.h"

#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "AutoStart.h"     // DescribeExecutionEnvironment()
#include "Config.h"        // GetModuleDirectory()
#include "Log.h"

namespace rck {

namespace {

// 监督循环的轮询间隔。会话登录/注销都不是"事件"，只能轮询；
// 1 秒足够及时，也不会给系统添负担。
constexpr DWORD kPollIntervalMs = 1000;

// 投递给用户会话的参数。--run 是默认行为，写上只为让日志里的命令行自解释。
constexpr wchar_t kHookArguments[] = L"--run";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS        g_status{};
HANDLE                g_stopEvent    = nullptr;

// 一个会话里的钩子子进程
struct SessionChild {
    DWORD  sessionId = 0;
    HANDLE process   = nullptr;   // 子进程句柄（用来等它退出）
    HANDLE job       = nullptr;   // KILL_ON_JOB_CLOSE 作业对象
};

std::vector<SessionChild> g_children;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring Win32ErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0,
                                     reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring text;
    if (len > 0 && buffer) {
        text.assign(buffer, len);
    }
    if (buffer) LocalFree(buffer);

    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    if (text.empty()) text = L"(无说明)";

    wchar_t codeText[32] = {};
    std::swprintf(codeText, 32, L"%lu", code);
    return std::wstring(codeText) + L" " + text;
}

std::wstring CurrentExePath() {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer)) return std::wstring();
    return buffer;
}

// 打开并启用一个特权。服务以 LocalSystem 运行时，
// SeTcbPrivilege / SeAssignPrimaryTokenPrivilege / SeIncreaseQuotaPrivilege
// 都**在令牌里**，但默认是关闭状态 —— CreateProcessAsUser 要求后两个必须启用。
bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    bool ok = false;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount           = 1;
        privileges.Privileges[0].Luid       = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        ok = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) != FALSE &&
             GetLastError() == ERROR_SUCCESS;
    }

    CloseHandle(token);
    return ok;
}

bool IsTokenElevated(HANDLE token) {
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    return GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE &&
           elevation.TokenIsElevated != 0;
}

// 令牌被 UAC 过滤（提权=否）时，换成它"链接"的完整令牌。
//
// 为什么需要：WTSQueryUserToken 给的是**会话的令牌**，在启用了 UAC 的机器上
// 有可能是被过滤的那一份（中完整性）。被过滤令牌的 TokenLinkedToken 指回
// 完整令牌 —— 系统服务有 SeTcbPrivilege，取得到。换成它之后投递进用户会话的
// 进程才是高完整性，钩子才拦得住送往提升窗口的按键。
//
// 换不了也不致命：照样启动，只是拦不到提升窗口的输入（日志里会记下来）。
void PreferElevatedToken(HANDLE& token) {
    if (IsTokenElevated(token)) return;

    TOKEN_LINKED_TOKEN linked{};
    DWORD returned = 0;
    if (!GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &returned)) {
        RCK_LOG_WARN(L"取链接令牌失败，按原令牌启动（可能拦不到提升窗口的按键）：%s",
                     Win32ErrorText(GetLastError()).c_str());
        return;
    }
    if (!linked.LinkedToken) return;

    if (!IsTokenElevated(linked.LinkedToken)) {
        RCK_LOG_WARN(L"链接令牌同样未提权，按原令牌启动（该用户可能不是管理员）");
        CloseHandle(linked.LinkedToken);
        return;
    }

    RCK_LOG_INFO(L"会话令牌被 UAC 过滤，已换用链接的完整令牌");
    CloseHandle(token);
    token = linked.LinkedToken;
}

// ---------------------------------------------------------------------------
// 会话枚举
// ---------------------------------------------------------------------------

// 当前**活动的交互式会话**编号（跳过 Session 0：那是服务会话，没有用户桌面）。
std::vector<DWORD> ActiveInteractiveSessions() {
    std::vector<DWORD> sessions;

    PWTS_SESSION_INFOW info  = nullptr;
    DWORD              count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count)) {
        RCK_LOG_ERROR(L"WTSEnumerateSessionsW 失败：%s", Win32ErrorText(GetLastError()).c_str());
        return sessions;
    }

    for (DWORD i = 0; i < count; ++i) {
        if (info[i].SessionId == 0) continue;              // Session 0 没有用户桌面
        if (info[i].State != WTSActive) continue;          // 只要已登录并正在使用的
        sessions.push_back(info[i].SessionId);
    }

    WTSFreeMemory(info);
    return sessions;
}

bool HasChild(DWORD sessionId) {
    for (const SessionChild& child : g_children) {
        if (child.sessionId == sessionId) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 把钩子进程投递进指定会话
// ---------------------------------------------------------------------------

bool LaunchHookInSession(DWORD sessionId) {
    const std::wstring exePath = CurrentExePath();
    if (exePath.empty()) {
        RCK_LOG_ERROR(L"取不到自身路径，无法投递钩子进程");
        return false;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        RCK_LOG_WARN(L"WTSQueryUserToken(会话 %lu) 失败：%s（下次轮询再试）",
                     sessionId, Win32ErrorText(GetLastError()).c_str());
        return false;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(userToken, TOKEN_ALL_ACCESS, nullptr,
                          SecurityImpersonation, TokenPrimary, &primaryToken)) {
        RCK_LOG_ERROR(L"DuplicateTokenEx 失败：%s", Win32ErrorText(GetLastError()).c_str());
        CloseHandle(userToken);
        return false;
    }
    CloseHandle(userToken);

    PreferElevatedToken(primaryToken);

    // 显式把令牌的会话号设成目标会话：CreateProcessAsUser 之后进程就落在
    // 那个会话的 WinSta0\Default 上，低级键盘钩子才盖得住该会话的输入。
    if (!SetTokenInformation(primaryToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
        RCK_LOG_WARN(L"SetTokenInformation(TokenSessionId=%lu) 失败：%s",
                     sessionId, Win32ErrorText(GetLastError()).c_str());
    }

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primaryToken, FALSE)) {
        RCK_LOG_WARN(L"CreateEnvironmentBlock 失败，子进程将不继承用户环境变量：%s",
                     Win32ErrorText(GetLastError()).c_str());
        environment = nullptr;
    }

    std::wstring commandLine = L"\"" + exePath + L"\" " + kHookArguments;

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.lpDesktop  = const_cast<LPWSTR>(L"winsta0\\default");
    startup.dwFlags    = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    const DWORD flags = environment ? CREATE_UNICODE_ENVIRONMENT : 0;

    const BOOL created = CreateProcessAsUserW(primaryToken, exePath.c_str(), commandLine.data(),
                                              nullptr, nullptr, FALSE, flags, environment,
                                              GetModuleDirectory().c_str(), &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

    if (environment) DestroyEnvironmentBlock(environment);
    CloseHandle(primaryToken);

    if (!created) {
        RCK_LOG_ERROR(L"CreateProcessAsUserW(会话 %lu) 失败：%s",
                      sessionId, Win32ErrorText(createError).c_str());
        if (createError == ERROR_PRIVILEGE_NOT_HELD) {   // 1314
            RCK_LOG_ERROR(L"提示：本服务缺少 SeAssignPrimaryTokenPrivilege / "
                          L"SeIncreaseQuotaPrivilege —— 确认服务以 LocalSystem 身份运行。");
        }
        if (createError == ERROR_ACCESS_DENIED) {        // 5
            RCK_LOG_ERROR(L"提示：令牌没有该会话的桌面权限，通常是会话尚未登录完成，"
                          L"下次轮询会重试。");
        }
        return false;
    }

    CloseHandle(process.hThread);

    // 作业对象：KILL_ON_JOB_CLOSE。服务被停止（或崩溃）时句柄随之关闭，
    // 子进程立刻被系统收掉，不会留下一个还在挂着钩子的孤儿进程。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            RCK_LOG_WARN(L"SetInformationJobObject 失败：%s", Win32ErrorText(GetLastError()).c_str());
        } else if (!AssignProcessToJobObject(job, process.hProcess)) {
            RCK_LOG_WARN(L"AssignProcessToJobObject 失败（子进程不会被服务停止连带结束）：%s",
                         Win32ErrorText(GetLastError()).c_str());
        }
    }

    g_children.push_back(SessionChild{sessionId, process.hProcess, job});

    RCK_LOG_INFO(L"已在会话 %lu 投递钩子进程 pid=%lu", sessionId, process.dwProcessId);
    RCK_BOOT(L"服务宿主投递钩子进程：会话=%lu pid=%lu", sessionId, process.dwProcessId);
    return true;
}

// ---------------------------------------------------------------------------
// 子进程同步
//
// 一次遍历同时干三件事：已经退出的收掉、会话消失的结束掉、缺会话的补上。
// ---------------------------------------------------------------------------

void SyncChildren(const std::vector<DWORD>& sessions, bool stopping) {
    for (auto it = g_children.begin(); it != g_children.end();) {
        const bool alive   = WaitForSingleObject(it->process, 0) != WAIT_OBJECT_0;
        const bool wanted  = !stopping &&
                             std::find(sessions.begin(), sessions.end(), it->sessionId) != sessions.end();

        if (alive && wanted) {
            ++it;
            continue;
        }

        if (alive) {
            const wchar_t* reason = stopping ? L"服务正在停止" : L"会话已不再活动";
            RCK_LOG_INFO(L"结束会话 %lu 的钩子进程 pid 未知（%s）", it->sessionId, reason);
            // 跨会话不能发窗口消息（窗口句柄是按会话隔离的），直接结束。
            // 钩子没有需要落盘的状态，退出即可。
            TerminateProcess(it->process, 0);
            WaitForSingleObject(it->process, 2000);
        }

        if (it->job) CloseHandle(it->job);
        CloseHandle(it->process);
        it = g_children.erase(it);
    }

    if (stopping) return;

    for (DWORD sessionId : sessions) {
        if (!HasChild(sessionId)) {
            LaunchHookInSession(sessionId);
        }
    }
}

// ---------------------------------------------------------------------------
// 服务状态上报
// ---------------------------------------------------------------------------

void ReportStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState     = state;
    g_status.dwWin32ExitCode    = win32ExitCode;
    g_status.dwWaitHint         = waitHint;
    g_status.dwControlsAccepted = (state == SERVICE_RUNNING)
                                  ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
                                  : 0;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        ++g_status.dwCheckPoint;
    } else {
        g_status.dwCheckPoint = 0;
    }

    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI HandlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        RCK_LOG_INFO(L"收到服务停止请求，正在结束各会话里的钩子进程");
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(g_status.dwCurrentState);
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ---------------------------------------------------------------------------
// 监督循环：每个活动会话一个钩子进程
// ---------------------------------------------------------------------------

void RunSupervisor() {
    RCK_LOG_INFO(L"服务宿主就绪，开始监督交互式会话（轮询间隔 %lu 毫秒）", kPollIntervalMs);
    RCK_BOOT(L"服务宿主就绪，开始监督交互式会话");

    for (;;) {
        const DWORD wait = WaitForSingleObject(g_stopEvent, kPollIntervalMs);
        if (wait != WAIT_TIMEOUT) break;

        SyncChildren(ActiveInteractiveSessions(), /*stopping=*/false);
    }

    SyncChildren({}, /*stopping=*/true);
    RCK_LOG_INFO(L"服务宿主已停止");
    RCK_BOOT(L"服务宿主已停止");
}

// ---------------------------------------------------------------------------
// SCM 回调
// ---------------------------------------------------------------------------

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, HandlerEx, nullptr);
    if (!g_statusHandle) {
        RCK_LOG_ERROR(L"RegisterServiceCtrlHandlerExW 失败：%s", Win32ErrorText(GetLastError()).c_str());
        return;
    }

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    RCK_LOG_INFO(L"==== 服务宿主启动 (pid=%lu) ====", GetCurrentProcessId());
    RCK_BOOT(L"服务宿主进程启动 pid=%lu 账户=%s",
             GetCurrentProcessId(), DescribeExecutionEnvironment().c_str());

    // CreateProcessAsUser / WTSQueryUserToken 需要的特权。缺了会导致
    // 投递失败（错误码 1314 / 5），所以这里逐个打开并记日志。
    struct PrivilegeRequest {
        const wchar_t* name;
        const wchar_t* label;
    };
    const PrivilegeRequest privileges[] = {
        { SE_TCB_NAME,                   L"SeTcb" },
        { SE_ASSIGNPRIMARYTOKEN_NAME,    L"SeAssignPrimaryToken" },
        { SE_INCREASE_QUOTA_NAME,        L"SeIncreaseQuota" },
        { SE_IMPERSONATE_NAME,           L"SeImpersonate" },
    };
    for (const PrivilegeRequest& privilege : privileges) {
        if (!EnablePrivilege(privilege.name)) {
            RCK_LOG_WARN(L"启用特权 %s 失败：%s", privilege.label, Win32ErrorText(GetLastError()).c_str());
        }
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        RCK_LOG_ERROR(L"CreateEventW 失败：%s", Win32ErrorText(GetLastError()).c_str());
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    ReportStatus(SERVICE_RUNNING);

    RunSupervisor();

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    ReportStatus(SERVICE_STOPPED);
    RCK_LOG_INFO(L"==== 服务宿主退出 ====");
}

} // namespace

int RunAsServiceHost(bool quiet) {
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr },
    };

    if (StartServiceCtrlDispatcherW(dispatchTable)) {
        return 0;
    }

    // 走到这里说明本进程不是被 SCM 启动的（最常见的错误码是 1063）。
    const DWORD error = GetLastError();
    RCK_LOG_ERROR(L"StartServiceCtrlDispatcherW 失败：%s", Win32ErrorText(error).c_str());
    RCK_BOOT(L"--service 模式启动失败：%s", Win32ErrorText(error).c_str());

    if (!quiet) {
        std::wstring body =
            L"本进程需要在「服务控制管理器」的管理下运行，不能直接双击启动。\n\n"
            L"请用配置程序里的「注册服务」按钮安装并启动它，"
            L"或在管理员命令行里执行：\n\n"
            L"    sc.exe start " + std::wstring(kServiceName) + L"\n\n"
            L"错误信息：\n" + Win32ErrorText(error);
        MessageBoxW(nullptr, body.c_str(), L"RemapCopilotKey", MB_OK | MB_ICONINFORMATION);
    }

    return 1;
}

} // namespace rck
