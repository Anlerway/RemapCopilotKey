// ---------------------------------------------------------------------------
// RemapCopilotKey - Copilot 键识别实现
// ---------------------------------------------------------------------------

#include "CopilotHook.h"

#include "Actions.h"
#include "Log.h"

namespace rck {

namespace {

// 三键序列的计时窗口。Copilot 键是固件级连发，通常在几毫秒内完成；
// 超过这个时间还没凑齐，就认为用户是按了真实的 Win / Shift。
constexpr UINT kChordTimeoutMs = 100;

// 真实的 Copilot 键 F23 扫描码（PS/2 集合 1）
constexpr WORD kScanCodeF23 = 0x6E;

// 目前只允许一个实例，钩子回调是 C 函数指针，需要一个全局入口
CopilotHook* g_instance = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

CopilotHook::~CopilotHook() {
    Uninstall();
}

bool CopilotHook::Install(HWND messageWindow, TriggerCallback callback) {
    Uninstall();

    m_window   = messageWindow;
    m_callback = std::move(callback);

    g_instance = this;
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &CopilotHook::StaticHookProc,
                               GetModuleHandleW(nullptr), 0);
    if (!m_hook) {
        g_instance = nullptr;
        RCK_LOG_ERROR(L"SetWindowsHookExW 失败（错误码 %lu）", GetLastError());
        return false;
    }

    m_pressState   = State::Idle;
    m_releaseState = State::Idle;
    m_leftWindowsSuppressed = false;
    m_leftShiftSuppressed   = false;

    RCK_LOG_INFO(L"键盘钩子已安装");
    return true;
}

void CopilotHook::Uninstall() {
    CancelChordTimer();
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
        RCK_LOG_INFO(L"键盘钩子已卸载");
    }
    if (g_instance == this) g_instance = nullptr;

    // 如果还有没补发的键，先补发出去，避免修饰键卡住
    if (HaveSuppressedKeys()) {
        ReplaySuppressedKeys();
    }
}

bool CopilotHook::Reinstall() {
    if (!m_window) return false;
    Uninstall();
    const bool ok = Install(m_window, m_callback);
    if (ok) RCK_LOG_INFO(L"键盘钩子已重新挂钩");
    return ok;
}

void CopilotHook::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;

    if (!m_enabled) {
        // 关闭时把吞掉的键放出去，并复位状态机
        CancelChordTimer();
        ReplaySuppressedKeys();
        m_pressState   = State::Idle;
        m_releaseState = State::Idle;
    }
    RCK_LOG_INFO(L"拦截功能已%s", enabled ? L"开启" : L"关闭");
}

bool CopilotHook::IsSequenceInProgress() const {
    return m_pressState != State::Idle || m_releaseState != State::Idle;
}

// ---------------------------------------------------------------------------
// 钩子回调
// ---------------------------------------------------------------------------

LRESULT CALLBACK CopilotHook::StaticHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (g_instance) {
        return g_instance->HookProc(code, wParam, lParam);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK CopilotHook::HookProc(int code, WPARAM wParam, LPARAM lParam) {
    // code < 0 表示必须原样传递，不能做任何处理
    if (code < 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const bool isInjected = (info->flags & LLKHF_INJECTED) != 0;
    const bool isExtended = (info->flags & LLKHF_EXTENDED) != 0;
    const bool isDown     = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    if (HandleKey(static_cast<WORD>(info->vkCode), static_cast<WORD>(info->scanCode),
                  isDown, isInjected, isExtended)) {
        return 1;   // 非零即吞掉这次按键，不再往下传
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

bool CopilotHook::HandleKey(WORD vkCode, WORD scanCode, bool isDown,
                            bool isInjected, bool isExtended) {
    if (!m_enabled) return false;

    // 自己注入的按键一律放行，避免自激
    if (isInjected) return false;

    // F23 优先按虚拟键码判断；若 F23 被注册表重映射过，则退回到扫描码判断
    const bool isF23 = (vkCode == VK_F23) || (scanCode == kScanCodeF23);

    if (isDown) {
        // ---------------- 按下序列 ----------------

        // 第一步：左 Win。无论当前处于什么状态，都从这里重新开始。
        if (vkCode == VK_LWIN) {
            ReplaySuppressedKeys();
            m_leftWindowsSuppressed = true;
            m_pressState = State::LeftWindows;
            EnsureChordTimer();
            return true;
        }

        // 第二步：左 Shift
        if (m_pressState == State::LeftWindows && vkCode == VK_LSHIFT) {
            m_leftShiftSuppressed = true;
            m_pressState = State::LeftShift;
            return true;
        }

        // 第三步：F23 —— 序列完成
        if (m_pressState == State::LeftShift && isF23) {
            m_pressState   = State::Idle;
            m_releaseState = State::F23;
            CancelChordTimer();
            m_leftShiftSuppressed   = false;
            m_leftWindowsSuppressed = false;

            RCK_LOG_INFO(L"识别到 Copilot 键按下");
            if (m_callback) m_callback(TriggerEvent::Press);
            return true;
        }

        // 其它按键打断了序列
        if (m_pressState != State::Idle) {
            m_pressState = State::Idle;
            CancelChordTimer();
            if (HaveSuppressedKeys()) {
                ReplaySuppressedKeys();
                // 先吞掉当前按键，再按正确顺序补发，
                // 保证其它程序看到的是 LWin -> LShift -> 本键
                PostInjectedKey(vkCode, false, isExtended);
                return true;
            }
        }
        return false;
    }

    // ---------------- 松开序列 ----------------
    // F23 -> LeftShift -> LeftWindows，这三个松开要吞掉

    if (m_releaseState == State::F23 && isF23) {
        m_releaseState = State::LeftShift;
        RCK_LOG_INFO(L"识别到 Copilot 键松开");
        if (m_callback) m_callback(TriggerEvent::Release);
        return true;
    }
    if (m_releaseState == State::LeftShift && vkCode == VK_LSHIFT) {
        m_releaseState = State::LeftWindows;
        return true;
    }
    if (m_releaseState == State::LeftWindows && vkCode == VK_LWIN) {
        m_releaseState = State::Idle;
        return true;
    }
    if (m_releaseState != State::Idle) {
        // 次序被打乱（例如 F23 已经松开但又松开了别的键），不再继续拦截
        m_releaseState = State::Idle;
    }

    // 按下序列中途松开：按原顺序补发被吞掉的键，
    // 并把这次松开也排到补发之后，避免出现"先松开后按下"的错误顺序
    if (m_pressState != State::Idle) {
        const bool lwinWasSuppressed   = m_leftWindowsSuppressed;
        const bool lshiftWasSuppressed = m_leftShiftSuppressed;

        m_pressState = State::Idle;
        CancelChordTimer();
        ReplaySuppressedKeys();

        if (lwinWasSuppressed && vkCode == VK_LWIN) {
            PostInjectedKey(vkCode, true, isExtended);
            return true;
        }
        if (lshiftWasSuppressed && vkCode == VK_LSHIFT) {
            PostInjectedKey(vkCode, true, isExtended);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

bool CopilotHook::HaveSuppressedKeys() const {
    return m_leftWindowsSuppressed || m_leftShiftSuppressed;
}

void CopilotHook::ReplaySuppressedKeys() {
    if (m_leftWindowsSuppressed) {
        m_leftWindowsSuppressed = false;
        PostInjectedKey(VK_LWIN, false, false);
    }
    if (m_leftShiftSuppressed) {
        m_leftShiftSuppressed = false;
        PostInjectedKey(VK_LSHIFT, false, false);
    }
    CancelChordTimer();
}

void CopilotHook::PostInjectedKey(WORD vkCode, bool keyUp, bool extended) {
    if (!m_window) return;
    LPARAM flags = 0;
    if (keyUp)    flags |= 1;
    if (extended) flags |= 2;
    PostMessageW(m_window, WM_RCK_SEND_INPUT, static_cast<WPARAM>(vkCode), flags);
}

void CopilotHook::EnsureChordTimer() {
    if (m_chordTimerActive || !m_window) return;
    SetTimer(m_window, kChordTimerId, kChordTimeoutMs, nullptr);
    m_chordTimerActive = true;
}

void CopilotHook::CancelChordTimer() {
    if (!m_chordTimerActive || !m_window) return;
    KillTimer(m_window, kChordTimerId);
    m_chordTimerActive = false;
}

void CopilotHook::OnChordTimeout() {
    m_chordTimerActive = false;

    if (m_pressState == State::Idle && !HaveSuppressedKeys()) return;

    RCK_LOG_DEBUG(L"三键序列超时（%u ms），补发被拦截的按键", kChordTimeoutMs);
    m_pressState = State::Idle;
    ReplaySuppressedKeys();
}

} // namespace rck
