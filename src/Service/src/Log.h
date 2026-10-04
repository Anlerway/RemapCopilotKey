#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 极简日志模块
//
// 目标：
//   * 不引入外部依赖
//   * 同时写文件与 OutputDebugString（便于用 DebugView 实时观察）
//   * 单文件超过阈值时自动轮转一次（保留 .1 备份）
//   * 线程安全（钩子回调与配置监视可能来自不同线程）
// ---------------------------------------------------------------------------

#include <string>

namespace rck {

enum class LogLevel { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4 };

// levelText 接受 "off" / "error" / "warn" / "info" / "debug"（大小写不敏感）
LogLevel LogLevelFromName(const std::string& levelText);
const wchar_t* LogLevelName(LogLevel level);

void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();

// 日志文件路径；传空字符串表示只输出到调试器，不写文件
void LogSetFile(const std::wstring& path);

void LogWrite(LogLevel level, const wchar_t* format, ...);

// ---------------------------------------------------------------------------
// 启动追踪：**不受 logLevel 影响**，永远写文件
//
// 为什么单独做一个：排查"重启后服务不起作用"时，第一件要知道的事是
// "进程到底走到哪一步了"。而这恰恰是普通日志最不可靠的场景 ——
// 用户机器上的 logLevel 可能是默认的 error，甚至配置根本没读成功，
// 于是启动过程一条记录都没有，只能靠猜。
//
// 启动追踪固定写到 <程序目录>\RemapCopilotKey.boot.log（每行带时间戳），
// 覆盖范围只有启动流程本身，量很小，不会长期增长。
// ---------------------------------------------------------------------------
void BootTraceSetFile(const std::wstring& path);
void BootTrace(const wchar_t* format, ...);

#define RCK_BOOT(...) ::rck::BootTrace(__VA_ARGS__)

#define RCK_LOG_ERROR(...) ::rck::LogWrite(::rck::LogLevel::Error, __VA_ARGS__)
#define RCK_LOG_WARN(...)  ::rck::LogWrite(::rck::LogLevel::Warn,  __VA_ARGS__)
#define RCK_LOG_INFO(...)  ::rck::LogWrite(::rck::LogLevel::Info,  __VA_ARGS__)
#define RCK_LOG_DEBUG(...) ::rck::LogWrite(::rck::LogLevel::Debug, __VA_ARGS__)

} // namespace rck
