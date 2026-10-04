// ---------------------------------------------------------------------------
// RemapCopilotKey - 虚拟键名表实现
// ---------------------------------------------------------------------------

#include "Keys.h"

#include <algorithm>
#include <cwchar>

namespace rck {

namespace {

// 说明：这里刻意不做成"按 vkey 下标索引的大数组"，而是显式的名字表。
// 好处是同一个 vkey 可以有多个别名（别名只用于解析，不用于显示），
// 而且新增键只需加一行，不会因为顺序错位而整体错乱。
const KeyInfo kKeys[] = {
    // --- 字母与数字 -------------------------------------------------------
    // WinUser.h 并不定义 VK_A..VK_Z，字母键的虚拟键码就是其大写 ASCII 值，
    //       所以这里直接写字面量。
    { L"VK_A", L'A' }, { L"VK_B", L'B' }, { L"VK_C", L'C' }, { L"VK_D", L'D' },
    { L"VK_E", L'E' }, { L"VK_F", L'F' }, { L"VK_G", L'G' }, { L"VK_H", L'H' },
    { L"VK_I", L'I' }, { L"VK_J", L'J' }, { L"VK_K", L'K' }, { L"VK_L", L'L' },
    { L"VK_M", L'M' }, { L"VK_N", L'N' }, { L"VK_O", L'O' }, { L"VK_P", L'P' },
    { L"VK_Q", L'Q' }, { L"VK_R", L'R' }, { L"VK_S", L'S' }, { L"VK_T", L'T' },
    { L"VK_U", L'U' }, { L"VK_V", L'V' }, { L"VK_W", L'W' }, { L"VK_X", L'X' },
    { L"VK_Y", L'Y' }, { L"VK_Z", L'Z' },

    { L"VK_0", L'0' }, { L"VK_1", L'1' }, { L"VK_2", L'2' }, { L"VK_3", L'3' },
    { L"VK_4", L'4' }, { L"VK_5", L'5' }, { L"VK_6", L'6' }, { L"VK_7", L'7' },
    { L"VK_8", L'8' }, { L"VK_9", L'9' },

    // --- 功能键 -----------------------------------------------------------
    { L"VK_F1",  VK_F1 },  { L"VK_F2",  VK_F2 },  { L"VK_F3",  VK_F3 },
    { L"VK_F4",  VK_F4 },  { L"VK_F5",  VK_F5 },  { L"VK_F6",  VK_F6 },
    { L"VK_F7",  VK_F7 },  { L"VK_F8",  VK_F8 },  { L"VK_F9",  VK_F9 },
    { L"VK_F10", VK_F10 }, { L"VK_F11", VK_F11 }, { L"VK_F12", VK_F12 },
    { L"VK_F13", VK_F13 }, { L"VK_F14", VK_F14 }, { L"VK_F15", VK_F15 },
    { L"VK_F16", VK_F16 }, { L"VK_F17", VK_F17 }, { L"VK_F18", VK_F18 },
    { L"VK_F19", VK_F19 }, { L"VK_F20", VK_F20 }, { L"VK_F21", VK_F21 },
    { L"VK_F22", VK_F22 }, { L"VK_F23", VK_F23 }, { L"VK_F24", VK_F24 },

    // --- 修饰键 -----------------------------------------------------------
    // 先给出"通用"修饰键（组合键里用它们最自然，系统会自行解析成左右键），
    // 再给出左右区分的版本。顺序不影响解析，规范名优先于别名。
    { L"VK_SHIFT",    VK_SHIFT },
    { L"VK_CONTROL",  VK_CONTROL },
    { L"VK_MENU",     VK_MENU },
    { L"VK_LSHIFT",   VK_LSHIFT },
    { L"VK_RSHIFT",   VK_RSHIFT },
    { L"VK_LCONTROL", VK_LCONTROL },
    { L"VK_RCONTROL", VK_RCONTROL },
    { L"VK_LMENU",    VK_LMENU },
    { L"VK_RMENU",    VK_RMENU },
    { L"VK_LWIN",     VK_LWIN },
    { L"VK_RWIN",     VK_RWIN },

    // --- 编辑 / 控制 ------------------------------------------------------
    { L"VK_BACK",     VK_BACK },
    { L"VK_TAB",      VK_TAB },
    { L"VK_RETURN",   VK_RETURN },
    { L"VK_ESCAPE",   VK_ESCAPE },
    { L"VK_SPACE",    VK_SPACE },
    { L"VK_CAPITAL",  VK_CAPITAL },
    { L"VK_APPS",     VK_APPS },
    { L"VK_SNAPSHOT", VK_SNAPSHOT },
    { L"VK_SCROLL",   VK_SCROLL },
    { L"VK_PAUSE",    VK_PAUSE },
    { L"VK_NUMLOCK",  VK_NUMLOCK },
    { L"VK_CLEAR",    VK_CLEAR },
    { L"VK_HELP",     VK_HELP },
    { L"VK_SELECT",   VK_SELECT },
    { L"VK_PRINT",    VK_PRINT },
    { L"VK_EXECUTE",  VK_EXECUTE },
    { L"VK_SLEEP",    VK_SLEEP },

    // --- 导航 -------------------------------------------------------------
    { L"VK_INSERT", VK_INSERT },
    { L"VK_DELETE", VK_DELETE },
    { L"VK_HOME",   VK_HOME },
    { L"VK_END",    VK_END },
    { L"VK_PRIOR",  VK_PRIOR },
    { L"VK_NEXT",   VK_NEXT },
    { L"VK_LEFT",   VK_LEFT },
    { L"VK_UP",     VK_UP },
    { L"VK_RIGHT",  VK_RIGHT },
    { L"VK_DOWN",   VK_DOWN },

    // --- 小键盘 -----------------------------------------------------------
    { L"VK_NUMPAD0",   VK_NUMPAD0 },
    { L"VK_NUMPAD1",   VK_NUMPAD1 },
    { L"VK_NUMPAD2",   VK_NUMPAD2 },
    { L"VK_NUMPAD3",   VK_NUMPAD3 },
    { L"VK_NUMPAD4",   VK_NUMPAD4 },
    { L"VK_NUMPAD5",   VK_NUMPAD5 },
    { L"VK_NUMPAD6",   VK_NUMPAD6 },
    { L"VK_NUMPAD7",   VK_NUMPAD7 },
    { L"VK_NUMPAD8",   VK_NUMPAD8 },
    { L"VK_NUMPAD9",   VK_NUMPAD9 },
    { L"VK_MULTIPLY",  VK_MULTIPLY },
    { L"VK_ADD",       VK_ADD },
    { L"VK_SUBTRACT",  VK_SUBTRACT },
    { L"VK_DECIMAL",   VK_DECIMAL },
    { L"VK_DIVIDE",    VK_DIVIDE },
    { L"VK_SEPARATOR", VK_SEPARATOR },

    // --- OEM / 符号 -------------------------------------------------------
    { L"VK_OEM_1",      VK_OEM_1 },
    { L"VK_OEM_PLUS",   VK_OEM_PLUS },
    { L"VK_OEM_COMMA",  VK_OEM_COMMA },
    { L"VK_OEM_MINUS",  VK_OEM_MINUS },
    { L"VK_OEM_PERIOD", VK_OEM_PERIOD },
    { L"VK_OEM_2",      VK_OEM_2 },
    { L"VK_OEM_3",      VK_OEM_3 },
    { L"VK_OEM_4",      VK_OEM_4 },
    { L"VK_OEM_5",      VK_OEM_5 },
    { L"VK_OEM_6",      VK_OEM_6 },
    { L"VK_OEM_7",      VK_OEM_7 },
    { L"VK_OEM_8",      VK_OEM_8 },
    { L"VK_OEM_102",    VK_OEM_102 },
    { L"VK_OEM_AX",     VK_OEM_AX },
    { L"VK_OEM_CLEAR",  VK_OEM_CLEAR },

    // --- 浏览器 / 媒体 ----------------------------------------------------
    { L"VK_BROWSER_BACK",      VK_BROWSER_BACK },
    { L"VK_BROWSER_FORWARD",   VK_BROWSER_FORWARD },
    { L"VK_BROWSER_REFRESH",   VK_BROWSER_REFRESH },
    { L"VK_BROWSER_STOP",      VK_BROWSER_STOP },
    { L"VK_BROWSER_SEARCH",    VK_BROWSER_SEARCH },
    { L"VK_BROWSER_FAVORITES", VK_BROWSER_FAVORITES },
    { L"VK_BROWSER_HOME",      VK_BROWSER_HOME },
    { L"VK_VOLUME_MUTE",       VK_VOLUME_MUTE },
    { L"VK_VOLUME_DOWN",       VK_VOLUME_DOWN },
    { L"VK_VOLUME_UP",         VK_VOLUME_UP },
    { L"VK_MEDIA_NEXT_TRACK",  VK_MEDIA_NEXT_TRACK },
    { L"VK_MEDIA_PREV_TRACK",  VK_MEDIA_PREV_TRACK },
    { L"VK_MEDIA_STOP",        VK_MEDIA_STOP },
    { L"VK_MEDIA_PLAY_PAUSE",  VK_MEDIA_PLAY_PAUSE },
    { L"VK_LAUNCH_MAIL",       VK_LAUNCH_MAIL },
    { L"VK_LAUNCH_MEDIA_SELECT", VK_LAUNCH_MEDIA_SELECT },
    { L"VK_LAUNCH_APP1",       VK_LAUNCH_APP1 },
    { L"VK_LAUNCH_APP2",       VK_LAUNCH_APP2 },
};

// 供解析用的别名（不参与 UI 显示、不参与反查）。
// 别名必须映射到"通用"修饰键，而不是右键版本 ——
// 否则 VK_CONTROL / VK_SHIFT 这类规范名会被别名抢先命中，
// 组合键就会被解析成 VK_RCONTROL / VK_RSHIFT（曾经踩过这个坑）。
const KeyInfo kAliases[] = {
    { L"CTRL",     VK_CONTROL },
    { L"CONTROL",  VK_CONTROL },
    { L"SHIFT",    VK_SHIFT },
    { L"ALT",      VK_MENU },
    { L"MENU",     VK_MENU },
    { L"WIN",      VK_LWIN },
    { L"LWIN",     VK_LWIN },
    { L"RCTRL",    VK_RCONTROL },
    { L"RCONTROL", VK_RCONTROL },
    { L"ENTER",    VK_RETURN },
    { L"RETURN",   VK_RETURN },
    { L"ESC",      VK_ESCAPE },
    { L"BACKSPACE", VK_BACK },
    { L"DEL",      VK_DELETE },
    { L"INS",      VK_INSERT },
    { L"PAGEUP",   VK_PRIOR },
    { L"PAGEDOWN", VK_NEXT },
    { L"PRTSC",    VK_SNAPSHOT },
    { L"PRINTSCREEN", VK_SNAPSHOT },
};

const std::vector<KeyInfo>& KeysVector() {
    static const std::vector<KeyInfo> v(std::begin(kKeys), std::end(kKeys));
    return v;
}

// 把输入的键名规范成大写并去掉空白
std::wstring Normalize(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        out.push_back(c);
    }
    return out;
}

} // namespace

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'a' && x <= L'z') x = static_cast<wchar_t>(x - L'a' + L'A');
        if (y >= L'a' && y <= L'z') y = static_cast<wchar_t>(y - L'a' + L'A');
        if (x != y) return false;
    }
    return true;
}

const std::vector<KeyInfo>& AllKeys() {
    return KeysVector();
}

WORD VKeyFromName(const std::wstring& name) {
    std::wstring key = Normalize(name);
    if (key.empty()) return 0;

    // 允许省略 "VK_" 前缀
    std::wstring bare = key;
    if (bare.size() > 3 && bare.compare(0, 3, L"VK_") == 0) {
        bare = bare.substr(3);
    }

    for (const KeyInfo& k : KeysVector()) {
        if (EqualsIgnoreCase(k.name, key)) return k.vkey;
        // 也接受 "VK_" 被省略的写法
        if (bare.size() + 3 == std::wcslen(k.name) &&
            EqualsIgnoreCase(k.name + 3, bare)) {
            return k.vkey;
        }
    }
    for (const KeyInfo& k : kAliases) {
        if (EqualsIgnoreCase(k.name, key) || EqualsIgnoreCase(k.name, bare)) {
            return k.vkey;
        }
    }

    // 单字母/单数字的特殊处理（"A"、"7"）
    if (key.size() == 1) {
        wchar_t c = key[0];
        if (c >= L'A' && c <= L'Z') return static_cast<WORD>(c);
        if (c >= L'0' && c <= L'9') return static_cast<WORD>(c);
    }
    return 0;
}

std::wstring VKeyName(WORD vkey) {
    for (const KeyInfo& k : KeysVector()) {
        if (k.vkey == vkey) return k.name;
    }
    wchar_t buf[16];
    std::swprintf(buf, 16, L"VK_0x%02X", vkey);
    return buf;
}

} // namespace rck
