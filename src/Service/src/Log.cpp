// ---------------------------------------------------------------------------
// RemapCopilotKey - 日志模块实现
// ---------------------------------------------------------------------------

#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

namespace rck {

namespace {

constexpr long long kMaxLogBytes = 1024 * 1024;   // 1 MB 后轮转

std::mutex  g_mutex;
// 默认只记错误：正常使用时用户不该被日志淹没。
// main.cpp 读完 config.json 后会按里面的 logLevel 覆盖这个值。
LogLevel    g_level = LogLevel::Error;
std::wstring g_path;
std::wstring g_bootPath;

std::wstring FormatTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[40];
    std::swprintf(buf, 40, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// 若日志文件超过阈值，把它重命名为 .1（覆盖旧备份）
void RotateIfNeeded() {
    if (g_path.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad)) return;

    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
    size.LowPart  = fad.nFileSizeLow;
    if (size.QuadPart < kMaxLogBytes) return;

    std::wstring backup = g_path + L".1";
    // MoveFileEx 带 REPLACE_EXISTING，即使备份已存在也能成功
    MoveFileExW(g_path.c_str(), backup.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void WriteLine(const std::wstring& line) {
    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");

    if (g_path.empty()) return;

    RotateIfNeeded();

    // 以 UTF-8 追加写入，方便直接用记事本/VS Code 查看中文
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string utf8(static_cast<size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                            utf8.data(), need, nullptr, nullptr);
        utf8 += "\r\n";
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(h);
}

} // namespace

LogLevel LogLevelFromName(const std::string& levelText) {
    std::string s;
    s.reserve(levelText.size());
    for (char c : levelText) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        s.push_back(c);
    }
    if (s == "off")   return LogLevel::Off;
    if (s == "error") return LogLevel::Error;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "debug") return LogLevel::Debug;
    return LogLevel::Info;
}

const wchar_t* LogLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Off:   return L"off";
    case LogLevel::Error: return L"error";
    case LogLevel::Warn:  return L"warn";
    case LogLevel::Info:  return L"info";
    case LogLevel::Debug: return L"debug";
    }
    return L"info";
}

void LogSetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel LogGetLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

void LogSetFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
}

void LogWrite(LogLevel level, const wchar_t* format, ...) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (level > g_level || g_level == LogLevel::Off) return;
    }

    wchar_t message[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    wchar_t prefix[16];
    std::swprintf(prefix, 16, L"[%-5s] ", LogLevelName(level));

    std::lock_guard<std::mutex> lock(g_mutex);
    WriteLine(FormatTimestamp() + L" " + prefix + message);
}

// ---------------------------------------------------------------------------
// 启动追踪
//
// 与普通日志的区别：**不看 logLevel、不做轮转**，永远往固定文件追加。
//
// 为什么需要：排查"开机自启没生效"时，用户机器上的 logLevel 多半还是默认的
// error，于是整个启动过程一条记录都没有 —— 而"进程到底走到哪一步了"
// 恰恰是这种情况下唯一的线索（尤其 Session 0 宿主这种东西，它连窗口都没有）。
// 覆盖范围只有启动流程本身，量很小，不会长期增长。
// ---------------------------------------------------------------------------

void BootTraceSetFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bootPath = path;
}

void BootTrace(const wchar_t* format, ...) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_bootPath.empty()) return;
        path = g_bootPath;
    }

    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    const std::wstring line = FormatTimestamp() + L" " + message;

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    const int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                         static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string utf8(static_cast<size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                            utf8.data(), need, nullptr, nullptr);
        utf8 += "\r\n";
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(h);
}

} // namespace rck
