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
//
// ★ 只在"孤立的左 Win"上下手（见设计说明 4.7）
//
//   笔记本厂商的 Fn 热键同样是以左 Win 开头的事件流，最常见的是
//   Fn+F5 -> Ctrl+Win+F24（开关触摸板）。这类事件流在按下 Win 时，
//   键盘上**已经有别的键按着**。
//
//   一旦把这种 Win 吞掉，后面只能靠 SendInput 补发 —— 而补发出来的是
//   **注入事件**（KBDLLHOOKSTRUCT.flags 带 LLKHF_INJECTED）。厂商的驱动 /
//   服务普遍会丢弃注入事件（防按键精灵那套逻辑），于是
//       Win 被吞 -> F24 被我们重新注入 -> 厂商热键彻底失效。
//
//   所以判定条件改成：只有当按下左 Win 时**键盘上没有任何其它键按着**，
//   才把它当成 Copilot 序列的起点；否则一个字节都不动，原样放行。
//   真实 Copilot 键是固件一次性连发的独立按键，永远满足这个条件。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <functional>
#include <vector>

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

    // 关闭时不拦截任何按键（配置里 enabled=false 时使用）。
    // 注意：物理按键状态的跟踪不受它影响，重新打开时状态依然准确。
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

    // 被吞掉、将来可能要原样补发的按键。附带扫描码与扩展位，
    // 这样即使 F23 被人用注册表 Scancode Map 改过，补发出去的仍是原键。
    struct SuppressedKey {
        WORD vk;
        WORD scan;
        bool extended;
    };

    static LRESULT CALLBACK StaticHookProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HookProc(int code, WPARAM wParam, LPARAM lParam);

    // 返回 true 表示吞掉这个按键
    bool HandleKey(WORD vkCode, WORD scanCode, bool isDown, bool isInjected, bool isExtended);

    // --- 物理按键状态 -------------------------------------------------------
    // 钩子里查询 GetAsyncKeyState 拿不到"这条事件之前"的状态，而且会被
    // 我们自己注入的按键污染，所以自己记账：每个键是否按着、除左 Win 外
    // 还按着几个键。这是"该不该抢左 Win"的唯一依据。
    void TrackPhysicalKey(WORD vkCode, bool isDown);
    bool CanStartChord() const;

    // --- 拦截/补发 ----------------------------------------------------------
    void SuppressKey(WORD vkCode, WORD scanCode, bool extended);
    bool WasSuppressed(WORD vkCode) const;
    bool HaveSuppressedKeys() const { return !m_suppressed.empty(); }
    void ReplaySuppressedKeys();
    void AbortChord();

    // --- 杂项 ---------------------------------------------------------------
    void ResetState();

    void EnsureChordTimer();
    void CancelChordTimer();

    void PostInjectedKey(WORD vkCode, bool keyUp, bool extended);

    HWND            m_window   = nullptr;
    HHOOK           m_hook     = nullptr;
    TriggerCallback m_callback;
    bool            m_enabled  = true;

    State m_pressState   = State::Idle;
    State m_releaseState = State::Idle;

    // 按下序列里已经被吞掉、等着补发的键（最多两个：左 Win、左 Shift）
    std::vector<SuppressedKey> m_suppressed;

    // 物理按键状态（只统计非注入事件）
    bool m_keyDown[256]  = {};
    int  m_otherKeysDown = 0;      // 除左 Win 之外按着的键数

    // 补发之后的一小段"不再抢左 Win"的冷却期，见 kReplayGuardMs
    DWORD m_passthroughUntil = 0;

    bool m_chordTimerActive = false;
};

} // namespace rck
