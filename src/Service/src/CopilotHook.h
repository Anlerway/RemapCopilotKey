#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - Copilot 键识别（低级键盘钩子 + 三键序列状态机）
//
// 背景（来自微软官方 "Windows Copilot Key Guidance for Ecosystem Partners"）：
//   Copilot 键在固件层就是三个真实按键，顺序固定：
//       按下： Left Windows -> Left Shift -> F23
//       松开： F23 -> Left Shift -> Left Windows
//   长按时三个键会按 Win/Shift/F23 的顺序重复上报。
//
//   所以任何"无条件替换扫描码"的方案都不成立（会漏掉 LWin 与 LShift），
//   必须做序列识别。
//
// 状态机：
//   按下序列  Idle -> LeftWindows -> LeftShift -> [完成]
//   松开序列  Idle -> F23 -> LeftShift -> LeftWindows -> Idle
//
//   打断按下序列的三种情况：其它键按下、任何键松开、100ms 定时器到期。
//   打断时按原顺序补发被吞掉的 LWin / LShift，保证其它程序看到的事件顺序正确。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <functional>

namespace rck {

enum class TriggerEvent { Press, Release };

using TriggerCallback = std::function<void(TriggerEvent)>;

class CopilotHook {
public:
    CopilotHook() = default;
    ~CopilotHook();

    CopilotHook(const CopilotHook&) = delete;
    CopilotHook& operator=(const CopilotHook&) = delete;

    // 安装全局键盘钩子。messageWindow 用于接收定时器与补发按键的消息。
    bool Install(HWND messageWindow, TriggerCallback callback);

    void Uninstall();

    // 重新挂钩子。多个程序都会装低级钩子时，钩子链的顺序会影响谁能先看到按键；
    // 遇到远程桌面之类的程序抢先处理时，可用这个重新抢到链首。
    bool Reinstall();

    bool IsInstalled() const { return m_hook != nullptr; }

    void SetCallback(TriggerCallback callback) { m_callback = std::move(callback); }

    // 关闭时不拦截任何按键（配置里 enabled=false 时使用）
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // 序列正在识别中（用于诊断/托盘提示）
    bool IsSequenceInProgress() const;

    // 由消息窗口在收到 WM_TIMER（三键超时）时转发
    void OnChordTimeout();

    // 定时器 ID，主窗口需要用它区分 WM_TIMER
    static constexpr UINT_PTR kChordTimerId = 1;

private:
    enum class State { Idle, LeftWindows, LeftShift, F23 };

    static LRESULT CALLBACK StaticHookProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HookProc(int code, WPARAM wParam, LPARAM lParam);

    // 返回 true 表示吞掉这个按键
    bool HandleKey(WORD vkCode, WORD scanCode, bool isDown, bool isInjected, bool isExtended);

    void ReplaySuppressedKeys();
    bool HaveSuppressedKeys() const;

    void EnsureChordTimer();
    void CancelChordTimer();

    void PostInjectedKey(WORD vkCode, bool keyUp, bool extended);

    HWND            m_window   = nullptr;
    HHOOK           m_hook     = nullptr;
    TriggerCallback m_callback;
    bool            m_enabled  = true;

    State m_pressState   = State::Idle;
    State m_releaseState = State::Idle;

    bool m_leftWindowsSuppressed = false;
    bool m_leftShiftSuppressed   = false;

    bool m_chordTimerActive = false;
};

} // namespace rck
