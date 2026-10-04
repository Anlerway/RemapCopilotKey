#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 动作执行器
//
// 关键设计：动作**不在键盘钩子回调里执行**。
// 钩子回调有超时限制，而且 SendInput 注入的按键会再次进入自己的钩子，
// 在回调里直接注入容易造成卡顿甚至重入。所以钩子只负责 PostMessage，
// 真正的注入/启动程序都在消息窗口的窗口过程里完成。
// ---------------------------------------------------------------------------

#include <windows.h>

#include "Config.h"

namespace rck {

// 由钩子投递、窗口过程处理的自定义消息
constexpr UINT WM_RCK_TRIGGER_DOWN = WM_APP + 1;
constexpr UINT WM_RCK_TRIGGER_UP   = WM_APP + 2;
// wParam = 虚拟键码；lParam 低位 = 1 表示松开，bit1 = 1 表示扩展键
constexpr UINT WM_RCK_SEND_INPUT   = WM_APP + 3;

class ActionExecutor {
public:
    // 触发键按下时调用（在消息窗口的窗口过程里）
    void HandlePress(const Action& action);

    // 触发键松开时调用
    void HandleRelease(const Action& action);

    // 强制释放仍然按下的目标键，避免修饰键卡住（例如切到管理员窗口时）
    void ForceReleaseAll();

    // 当前是否有由本程序注入且未松开的按键
    bool IsKeyHeld() const { return m_keyDown; }

private:
    bool m_keyDown = false;
    WORD m_key     = 0;
};

// 注入单个按键（带正确的扫描码与扩展位）。返回是否成功。
// extended 由调用方明确给出（例如从钩子结构里取回原始标志）；
// 传 false 时函数会按虚拟键码自动判断常用扩展键。
bool SendKeyInput(WORD vkey, bool keyUp, bool extended = false);

// 注入一个组合键：先按顺序按下，再逆序松开
bool SendChordInput(const std::vector<WORD>& keys);

// 虚拟键码 -> 扫描码，并修正 MapVirtualKey 在方向键/小键盘/PrintScreen 上的歧义
UINT VKeyToScanCode(WORD vkey, bool extended);

// 该虚拟键码是否必须带扩展位（否则会被当成小键盘上的另一个键）
bool IsAlwaysExtendedKey(WORD vkey);

// 打开一个文件 / 程序 / 协议链接（ShellExecute）
// runAsAdmin=true 时使用 "runas" 动词，被打开的程序会以管理员权限启动
bool ShellOpen(const std::wstring& target, const std::wstring& arguments,
               bool runAsAdmin = false);

// 按可执行文件名查找正在运行的进程，返回其主窗口；找不到返回 nullptr
HWND FindRunningAppWindow(const std::wstring& exePathOrName);

// 切换窗口状态：已最小化则还原并激活；已是前台则最小化
void ToggleAppWindow(HWND hwnd);

// 请求关闭窗口（WM_CLOSE），成功返回 true
bool CloseAppWindow(HWND hwnd);

// 把 Windows 实用功能展开为实际动作
void ResolveSystemAction(SystemActionId id, std::wstring& target, std::wstring& arguments,
                         std::vector<WORD>& chord);

} // namespace rck
