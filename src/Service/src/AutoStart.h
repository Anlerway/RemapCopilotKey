#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 权限与当前用户身份
//
// 注意：**开机自启（计划任务）不在这个模块里，也不在服务程序里。**
// 它由配置程序负责注册与删除，实现见 src/Configurator/AutoStart.cs。
//
// ── 自启用的是哪一种任务 ────────────────────────────────────────────────
//
// "登录时"触发 + 主体是**当前用户 SID** + `LogonType=InteractiveToken`
// + `RunLevel=HighestAvailable`。勾子进程就跑在用户交互式会话里，
// 由计划任务静默提权启动，不弹 UAC。
//
// ── 试过并放弃的三条路（都别再走回去）──────────────────────────────────
//
//  1. 任务定义里写 `<RequiredPrivileges>`：v1.3 元素，只对 LOCAL SERVICE /
//     NETWORK SERVICE 的主体有效，主体是登录用户时校验器直接拒绝，
//     `schtasks /Create /XML` 必然失败、注册永远成功不了。
//  2. 主体写 `GroupId`（或干脆用 `schtasks /Create /SC ONLOGON` 不带 /RU）：
//     计划任务程序会把登录方式锁成 `TASK_LOGON_GROUP`（按组激活），
//     任务进程被打上**低完整性(0x1000)** 标签 —— 钩子装得上、日志照写，
//     但一个按键都收不到。
//  3. "系统启动时 + SYSTEM 任务 + Session 0 宿主 + CreateProcessAsUser 投递"：
//     本来是想绕开 UAC，但**计划任务程序给任务进程发的是低完整性受限令牌，
//     连 SYSTEM 主体也一样**。实测现场：
//
//         宿主身份: 账户=NT AUTHORITY\SYSTEM 会话=0 ... 完整性=低(0x1000) 提权=是
//         特权: SeTcb=1 SeAssignPrimaryToken=1 SeIncreaseQuota=1
//         WTSQueryUserToken(会话 1) 失败：错误码 5        ← ERROR_ACCESS_DENIED
//
//     账户是 SYSTEM、特权也开着，照样拒绝 —— 受限令牌下调不动
//     WTSQueryUserToken。要真走这条路，载体必须换成 SCM 注册的 Windows 服务
//     （服务登录方式给的是完整 SYSTEM 令牌），而不是计划任务。
//
// 判断自启是否真的生效，只看一个地方：日志里 DescribeExecutionEnvironment()
// 那一行必须是「会话=1 窗口站=WinSta0 桌面=Default 完整性=高(0x3000)」。
// （这一行同时写进 RemapCopilotKey.boot.log，不受 logLevel 影响。）
// ---------------------------------------------------------------------------

#include <string>

namespace rck {

// 当前进程是否以管理员权限运行（用于启动时提示与诊断）
bool IsRunningElevated();

// 进程运行环境描述：会话 / 窗口站 / 桌面 / 完整性级别。
//
// 为什么需要它：低级键盘钩子**只能看到与它同一个桌面的输入**。
// 如果进程被放进别的会话、别的窗口站或别的桌面（由计划任务、服务宿主或
// 某些启动器拉起时就可能这样），SetWindowsHookExW 会成功返回，
// 但一个按键都收不到 —— 没有错误码，只能靠把这些信息写进日志来区分。
std::wstring DescribeExecutionEnvironment();

} // namespace rck
