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

// 补发过一次被吞掉的按键之后，这么长时间内不再抢左 Win。
//
// 这一条是给"抢错了"兜底的：厂商热键是硬件一次性连发，我们如果判断失误
// 吞掉了其中的 Win，补发出去的是注入事件、厂商服务不认，这一次热键就废了。
// 有了冷却期，至少不会紧接着把同一次动作的后续事件（长按重复）再废一遍，
// 也不会让"按住热键"变成每次重复都失效。
constexpr DWORD kReplayGuardMs = 250;

// 真实的 Copilot 键 F23 扫描码（PS/2 集合 1）
constexpr WORD kScanCodeF23 = 0x6E;

// 目前只允许一个实例，钩子回调是 C 函数指针，需要一个全局入口
CopilotHook* g_instance = nullptr;

// GetTickCount 会回绕，用有符号差值比较（±24.8 天内安全），不要直接比大小
bool Reached(DWORD now, DWORD deadline) {
    return static_cast<LONG>(now - deadline) >= 0;
}

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

    ResetState();

    RCK_LOG_INFO(L"键盘钩子已安装");
    return true;
}

void CopilotHook::Uninstall() {
    CancelChordTimer();

    // 如果还有没补发的键，先补发出去，避免修饰键卡住
    if (HaveSuppressedKeys()) {
        ReplaySuppressedKeys();
    }

    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
        RCK_LOG_INFO(L"键盘钩子已卸载");
    }
    if (g_instance == this) g_instance = nullptr;

    ResetState();
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
        if (HaveSuppressedKeys()) ReplaySuppressedKeys();
        ResetState();
    }
    RCK_LOG_INFO(L"拦截功能已%s", enabled ? L"开启" : L"关闭");
}

bool CopilotHook::IsSequenceInProgress() const {
    return m_pressState != State::Idle || m_releaseState != State::Idle;
}

void CopilotHook::ResetState() {
    m_pressState   = State::Idle;
    m_releaseState = State::Idle;
    m_suppressed.clear();

    for (bool& down : m_keyDown) down = false;
    m_otherKeysDown    = 0;
    m_passthroughUntil = 0;
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
    // 自己注入的按键一律放行，避免自激；它们也不参与物理按键状态的统计
    if (isInjected) return false;

    // 先记账：不管这个键最后吞不吞，它确实在物理上被按下 / 松开了。
    // 这一步必须在 enabled 判断之外，否则开关切换后状态会失真。
    TrackPhysicalKey(vkCode, isDown);

    if (!m_enabled) return false;

    // F23 优先按虚拟键码判断；若 F23 被注册表重映射过，则退回到扫描码判断
    const bool isF23 = (vkCode == VK_F23) || (scanCode == kScanCodeF23);

    if (isDown) {
        // ---------------- 按下序列 ----------------

        if (m_pressState != State::Idle) {
            // 序列进行中。固件长按会把 Win / Shift / F23 按顺序重复上报，
            // 中途漏出任何一个都会让系统看到真实的 Win / Shift，所以重复也吞掉。
            if (vkCode == VK_LWIN) {
                return true;                                // 重复的左 Win
            }
            if (vkCode == VK_LSHIFT) {
                if (m_pressState == State::LeftWindows) {
                    SuppressKey(vkCode, scanCode, isExtended);
                    m_pressState = State::LeftShift;
                }
                return true;                                // 重复的左 Shift
            }
            if (m_pressState == State::LeftShift && isF23) {
                // 第三步：F23 —— 序列完成
                m_pressState   = State::Idle;
                m_releaseState = State::F23;
                CancelChordTimer();
                m_suppressed.clear();   // 三个键都已吞掉，不需要补发

                RCK_LOG_INFO(L"识别到 Copilot 键按下");
                if (m_callback) m_callback(TriggerEvent::Press);
                return true;
            }

            // 其它按键打断了序列（用户其实是在按 Win+E、Win+R ……）
            RCK_LOG_DEBUG(L"三键序列被打断（vk=0x%02X），补发被拦截的按键", vkCode);
            AbortChord();
            // 补发的左 Win 已经排进消息队列，当前键排在它之后，
            // 保证其它程序看到的是 LWin -> 本键（直接放行会变成"先本键后 Win"）
            PostInjectedKey(vkCode, false, isExtended);
            return true;
        }

        // ---------------- 序列起点：左 Win ----------------
        if (vkCode == VK_LWIN) {
            if (!CanStartChord()) {
                // 不是孤立的按下：厂商热键（Ctrl+Win+F24 等）、用户的
                // Win+Shift+S，或者刚刚补发过一次。原样放行，一个字节都不动。
                return false;
            }

            SuppressKey(vkCode, scanCode, isExtended);
            m_pressState = State::LeftWindows;
            EnsureChordTimer();
            return true;
        }

        return false;
    }

    // ---------------- 松开序列 ----------------
    // F23 -> LeftShift -> LeftWindows，这三个松开要吞掉

    if (m_releaseState != State::Idle) {
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
        // 次序被打乱（例如 F23 已经松开但又松开了别的键），不再继续拦截，
        // 宁可多放行也不要卡键
        m_releaseState = State::Idle;
    }

    // 按下序列中途松开：按原顺序补发被吞掉的键，
    // 并把这次松开也排到补发之后，避免出现"先松开后按下"的错误顺序
    if (m_pressState != State::Idle) {
        const bool wasSuppressed = WasSuppressed(vkCode);
        AbortChord();
        if (wasSuppressed) {
            PostInjectedKey(vkCode, true, isExtended);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 物理按键状态
// ---------------------------------------------------------------------------

void CopilotHook::TrackPhysicalKey(WORD vkCode, bool isDown) {
    if (vkCode >= 256) return;

    const bool wasDown = m_keyDown[vkCode];
    if (isDown == wasDown) return;          // 自动重复不是新的按下

    m_keyDown[vkCode] = isDown;

    // 左 Win 自己不算"别的键"：它正是要被判定为起点的那一个
    if (vkCode == VK_LWIN) return;

    if (isDown) {
        ++m_otherKeysDown;
    } else if (m_otherKeysDown > 0) {
        --m_otherKeysDown;
    }
}

bool CopilotHook::CanStartChord() const {
    // 1) 键盘上不能有别的键按着。
    //
    //    Copilot 键是固件一次性连发的独立按键，按下它时手上不可能同时按着
    //    别的键；而厂商热键（Fn+F5 -> Ctrl+Win+F24）和用户的组合键
    //    （Win+Shift+S、Ctrl+Win+左/右箭头 ……）都是先按住修饰键再发 Win。
    //    这条判断把那些 Win 全部挡在门外，一个都不吞 —— 也就根本不需要
    //    用注入事件去"还原"它们。
    if (m_otherKeysDown != 0) return false;

    // 2) 刚刚补发过一次就不再抢：一次误判只让那一次热键失效，不连锁。
    return Reached(GetTickCount(), m_passthroughUntil);
}

// ---------------------------------------------------------------------------
// 拦截与补发
// ---------------------------------------------------------------------------

void CopilotHook::SuppressKey(WORD vkCode, WORD scanCode, bool extended) {
    m_suppressed.push_back(SuppressedKey{vkCode, scanCode, extended});
}

bool CopilotHook::WasSuppressed(WORD vkCode) const {
    for (const SuppressedKey& key : m_suppressed) {
        if (key.vk == vkCode) return true;
    }
    return false;
}

void CopilotHook::ReplaySuppressedKeys() {
    if (m_suppressed.empty()) {
        CancelChordTimer();
        return;
    }

    RCK_LOG_DEBUG(L"补发被拦截的按键（%u 个），此后 %lu ms 内不再拦截左 Win",
                  static_cast<unsigned>(m_suppressed.size()),
                  static_cast<unsigned long>(kReplayGuardMs));

    // 顺序很重要：按下序列里先吞的是左 Win，后吞的是左 Shift
    for (const SuppressedKey& key : m_suppressed) {
        PostInjectedKey(key.vk, false, key.extended);
    }
    m_suppressed.clear();
    CancelChordTimer();
}

void CopilotHook::AbortChord() {
    m_pressState   = State::Idle;
    m_releaseState = State::Idle;
    ReplaySuppressedKeys();
    m_passthroughUntil = GetTickCount() + kReplayGuardMs;
}

void CopilotHook::PostInjectedKey(WORD vkCode, bool keyUp, bool extended) {
    if (!m_window) return;
    LPARAM flags = 0;
    if (keyUp)    flags |= 1;
    if (extended) flags |= 2;
    PostMessageW(m_window, WM_RCK_SEND_INPUT, static_cast<WPARAM>(vkCode), flags);
}

// ---------------------------------------------------------------------------
// 定时器
// ---------------------------------------------------------------------------

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
    AbortChord();
}

} // namespace rck
