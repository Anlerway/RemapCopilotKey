#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - SCM 服务宿主（把钩子进程投递到用户会话）
//
// ── 为什么需要这一层 ────────────────────────────────────────────────────
//
// 低级键盘钩子（WH_KEYBOARD_LL）必须跑在**用户交互式会话**里，而且必须是
// **高完整性**（否则受 UIPI 限制，看不到送往提升窗口的按键）。
//
// 之前用计划任务以 RunLevel=HighestAvailable 启动用户会话里的实例，本该
// 拿到高完整性，但实测拿到的却是低完整性(0x1000)受限令牌（提权=是、完整性=低），
// 钩子装得上、日志照写、一个按键都收不到。这条路径已放弃 —— 详见
// docs/设计说明.md 第 8 节。
//
// 现在改成 Windows 服务的标准做法：
//
//   1. SCM 以 LocalSystem 启动本程序（--service）。服务令牌由 SCM 用服务登录
//      方式创建，是**完整 SYSTEM 令牌**，不经过计划任务那套令牌硬化。
//   2. 服务每秒钟枚举一次交互式会话（WTSEnumerateSessions，跳过 Session 0），
//      对每个状态为 Active 的会话：
//        WTSQueryUserToken → DuplicateTokenEx → （被 UAC 过滤时换用
//        TokenLinkedToken 那份完整令牌）→ SetTokenInformation(TokenSessionId)
//        → CreateEnvironmentBlock → CreateProcessAsUserW(winsta0\default)
//      把同一个 exe 以 --run 投递进该会话。
//   3. 每个子进程挂进一个 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 的作业对象，
//      服务停止/崩溃时子进程随之收掉，不会留下孤儿。
//
// 于是登录后自动得到「会话=N 窗口站=WinSta0 桌面=Default 完整性=高(0x3000)」，
// 且**全程不弹 UAC**。
//
// 判断是否生效只看一处：boot.log 里 DescribeExecutionEnvironment() 那一行。
// ---------------------------------------------------------------------------

namespace rck {

// SCM 服务名（注册、查询、删除都用它；只用英文数字，便于 sc.exe 手动操作）。
inline constexpr wchar_t kServiceName[] = L"RemapCopilotKeyAgent";

// 以 Windows 服务方式运行（由 SCM 启动）。quiet 为 true 时不弹任何对话框。
// 返回进程退出码。
int RunAsServiceHost(bool quiet);

} // namespace rck
