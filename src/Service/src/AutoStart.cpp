// ---------------------------------------------------------------------------
// RemapCopilotKey - 权限与当前用户身份
//
// 这个模块**不注册开机自启**。登录自启的计划任务由配置程序负责，
// 实现见 src/Configurator/AutoStart.cs（那里用的是 schtasks 的命令行开关）。
//
// 为什么自启放在配置程序而不是这里：
//
//   * 配置程序与服务程序同级提权运行，自己就能注册计划任务，
//     不必再 ShellExecute runas 拉起服务程序执行安装参数；
//   * 注册属于"配置"，和键盘钩子的运行逻辑是两件事，边界更清楚。
//
// 这里只保留服务程序启动时要用到的两项查询：自己有没有提权，
// 以及自己落在哪个会话 / 窗口站 / 桌面上。
// ---------------------------------------------------------------------------

#include "AutoStart.h"

#include <windows.h>

#include <vector>

namespace rck {

// ---------------------------------------------------------------------------
// 权限与身份
// ---------------------------------------------------------------------------

bool IsRunningElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
// 运行环境描述
// ---------------------------------------------------------------------------

namespace {

// 取某个内核对象的名称（窗口站 / 桌面都用它）
std::wstring UserObjectName(HANDLE handle) {
    if (!handle) return L"(取不到)";

    DWORD needed = 0;
    GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &needed);
    if (needed == 0) return L"(取不到)";

    std::vector<wchar_t> buffer(needed / sizeof(wchar_t) + 1, L'\0');
    if (!GetUserObjectInformationW(handle, UOI_NAME, buffer.data(),
                                   static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                   &needed)) {
        return L"(取不到)";
    }

    return buffer.data();
}

// 进程令牌的完整性级别 RID：0x1000 中、0x2000 高、0x3000 系统
DWORD IntegrityRid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;

    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return 0;
    }

    std::vector<BYTE> buffer(size);
    const BOOL ok = GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) return 0;

    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const DWORD* rid = GetSidSubAuthority(label->Label.Sid,
                                          *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    return rid ? *rid : 0;
}

// 完整性级别的 RID 取值（**别写错，这里写错会把人带到完全错误的方向**）：
//   Low = 0x1000 (4096)、Medium = 0x2000 (8192)、High = 0x3000 (12288)、System = 0x4000 (16384)
// 注意 NOT 0x1000/0x2000/0x3000/0x4000 对应 UN/低/中/高 —— 早期把这一档整体写少了一位，
// 于是把"高完整性"报成了"系统"，导致排查方向整个跑偏。
const wchar_t* IntegrityName(DWORD rid) {
    switch (rid) {
    case 0x0000: return L"不受信任";
    case 0x1000: return L"低";
    case 0x2000: return L"中";
    case 0x3000: return L"高";
    case 0x4000: return L"系统";
    default:     return L"未知";
    }
}

// 进程令牌里的账户名（形如 LAPTOP\ASUS 或 NT AUTHORITY\SYSTEM）。
// 这一项决定了"服务是以谁的身份在跑"，是排查自启问题的第一现场信息。
std::wstring TokenUserName() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"(取不到)";

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) { CloseHandle(token); return L"(取不到)"; }

    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return L"(取不到)";
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t name[256] = {};
    wchar_t domain[256] = {};
    DWORD nameLen = 256, domainLen = 256;
    SID_NAME_USE use{};

    const BOOL ok = LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen,
                                      domain, &domainLen, &use);
    // 无论查询成功与否都要关掉令牌
    CloseHandle(token);
    if (!ok) return L"(取不到)";

    return std::wstring(domain) + L"\\" + name;
}

} // namespace

std::wstring DescribeExecutionEnvironment() {
    // 窗口站与桌面：钩子只能看到**同一个桌面**上的输入。
    // 这两项是判断"进程有没有在用户的活动桌面上"的唯一依据。
    //
    // 注意 GetThreadDesktop 返回的句柄不需要（也不能）释放，
    // 而 OpenWindowStationW 打开的句柄必须释放。
    const std::wstring desktopName  = UserObjectName(GetThreadDesktop(GetCurrentThreadId()));

    std::wstring stationName = L"(取不到)";
    HWINSTA station = OpenWindowStationW(L"WinSta0", FALSE, WINSTA_READATTRIBUTES);
    if (station) {
        stationName = UserObjectName(station);
        CloseWindowStation(station);
    }

    const DWORD rid = IntegrityRid();

    // 会话号：低级键盘钩子只覆盖同一个会话里的输入
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) sessionId = 0xFFFFFFFF;

    wchar_t buffer[512] = {};
    std::swprintf(buffer, 512,
                  L"账户=%s 会话=%lu 窗口站=%s 桌面=%s 完整性=%s(0x%lX) 提权=%s",
                  TokenUserName().c_str(),
                  sessionId,
                  stationName.c_str(),
                  desktopName.c_str(),
                  IntegrityName(rid), rid,
                  IsRunningElevated() ? L"是" : L"否");
    return buffer;
}

} // namespace rck
