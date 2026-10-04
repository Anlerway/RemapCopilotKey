// ---------------------------------------------------------------------------
// RemapCopilotKey - 配置模型实现
// ---------------------------------------------------------------------------

#include "Config.h"

#include "Json.h"
#include "Keys.h"
#include "Log.h"

#include <cstdio>
#include <cstring>

namespace rck {

namespace {

constexpr wchar_t kConfigFileName[] = L"config.json";
constexpr wchar_t kLogFileName[]    = L"RemapCopilotKeyService.log";

// --- 字符串转换 -----------------------------------------------------------

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

// --- JSON -> 配置 的小工具 -------------------------------------------------

std::wstring GetWideString(const json::Value& obj, const char* key, const std::wstring& fallback) {
    const json::Value* v = obj.find(key);
    if (!v || !v->isString()) return fallback;
    return Utf8ToWide(v->asString());
}

bool GetBool(const json::Value& obj, const char* key, bool fallback) {
    const json::Value* v = obj.find(key);
    if (!v || !v->isBool()) return fallback;
    return v->asBool();
}

// --- 动作 <-> JSON --------------------------------------------------------

json::Value ActionToJson(const Action& action) {
    json::Value obj = json::Value::MakeObject();
    obj.set("type", json::Value(WideToUtf8(ActionTypeName(action.type))));

    switch (action.type) {
    case ActionType::SendKey:
        obj.set("vkey", json::Value(WideToUtf8(VKeyName(action.vkey))));
        break;

    case ActionType::SendChord: {
        json::Value keys = json::Value::MakeArray();
        for (WORD vk : action.chord) {
            keys.push(json::Value(WideToUtf8(VKeyName(vk))));
        }
        obj.set("keys", keys);
        break;
    }

    case ActionType::OpenApp:
        obj.set("path", json::Value(WideToUtf8(action.path)));
        obj.set("args", json::Value(WideToUtf8(action.arguments)));
        obj.set("runAsAdmin", json::Value(action.runAsAdmin));
        obj.set("toggleWindow", json::Value(action.toggleWindow));
        break;

    case ActionType::OpenUrl:
        obj.set("url", json::Value(WideToUtf8(action.url)));
        break;

    case ActionType::SystemAction:
        obj.set("action", json::Value(WideToUtf8(SystemActionName(action.systemAction))));
        break;
    }
    return obj;
}

void ActionFromJson(const json::Value& node, Action& out, std::wstring& warnings) {
    if (!node.isObject()) {
        warnings += L"mapping.action 不是对象，已使用默认动作。";
        return;
    }

    const std::wstring typeName = GetWideString(node, "type", L"sendKey");
    ActionType type = ActionType::SendKey;
    if (!ActionTypeFromName(typeName, type)) {
        warnings += L"未知的动作类型 \"" + typeName + L"\"，已使用默认动作。";
        return;
    }
    out.type = type;

    switch (type) {
    case ActionType::SendKey: {
        const std::wstring vkeyName = GetWideString(node, "vkey", L"VK_RCONTROL");
        const WORD vk = VKeyFromName(vkeyName);
        if (vk == 0) {
            warnings += L"未知的按键 \"" + vkeyName + L"\"，已回退为 VK_RCONTROL。";
            out.vkey = VK_RCONTROL;
        } else {
            out.vkey = vk;
        }
        break;
    }

    case ActionType::SendChord: {
        out.chord.clear();
        const json::Value* keys = node.find("keys");
        if (keys && keys->isArray()) {
            for (const json::Value& item : keys->items()) {
                if (!item.isString()) continue;
                const std::wstring name = Utf8ToWide(item.asString());
                const WORD vk = VKeyFromName(name);
                if (vk == 0) {
                    warnings += L"组合键中忽略了未知按键 \"" + name + L"\"。";
                } else {
                    out.chord.push_back(vk);
                }
            }
        }
        if (out.chord.empty()) {
            warnings += L"组合键为空，已回退为 Ctrl+Shift+Escape。";
            out.chord = { VK_CONTROL, VK_SHIFT, VK_ESCAPE };
        }
        break;
    }

    case ActionType::OpenApp:
        out.path         = GetWideString(node, "path", L"");
        out.arguments    = GetWideString(node, "args", L"");
        out.runAsAdmin   = GetBool(node, "runAsAdmin", false);
        out.toggleWindow = GetBool(node, "toggleWindow", false);
        if (out.path.empty()) warnings += L"openApp 的 path 为空，触发时不会有任何效果。";
        break;

    case ActionType::OpenUrl:
        out.url = GetWideString(node, "url", L"");
        if (out.url.empty()) warnings += L"openUrl 的 url 为空，触发时不会有任何效果。";
        break;

    case ActionType::SystemAction: {
        const std::wstring name = GetWideString(node, "action", L"explorer");
        SystemActionId id = SystemActionId::Explorer;
        if (!SystemActionFromName(name, id)) {
            warnings += L"未知的实用功能 \"" + name + L"\"，已回退为「资源管理器」。";
        } else {
            out.systemAction = id;
        }
        break;
    }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 动作类型名称
// ---------------------------------------------------------------------------

const wchar_t* ActionTypeName(ActionType type) {
    switch (type) {
    case ActionType::SendKey:      return L"sendKey";
    case ActionType::SendChord:    return L"sendChord";
    case ActionType::OpenApp:      return L"openApp";
    case ActionType::OpenUrl:      return L"openUrl";
    case ActionType::SystemAction: return L"systemAction";
    }
    return L"sendKey";
}

bool ActionTypeFromName(const std::wstring& name, ActionType& out) {
    const std::wstring k = ToLower(name);
    if (k == L"sendkey" || k == L"key")            { out = ActionType::SendKey;      return true; }
    if (k == L"sendchord" || k == L"chord")        { out = ActionType::SendChord;    return true; }
    if (k == L"openapp" || k == L"app")            { out = ActionType::OpenApp;      return true; }
    if (k == L"openurl" || k == L"url")            { out = ActionType::OpenUrl;      return true; }
    if (k == L"systemaction" || k == L"system")    { out = ActionType::SystemAction; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// 实用功能名称
// ---------------------------------------------------------------------------

namespace {
const struct { const wchar_t* name; SystemActionId id; } kSystemActions[] = {
    { L"explorer",         SystemActionId::Explorer },
    { L"taskManager",      SystemActionId::TaskManager },
    { L"cmd",              SystemActionId::Cmd },
    { L"powershell",       SystemActionId::PowerShell },
    { L"screenshot",       SystemActionId::Screenshot },
    { L"snippingTool",     SystemActionId::SnippingTool },
    { L"settings",         SystemActionId::Settings },
    { L"controlPanel",     SystemActionId::ControlPanel },
    { L"calculator",       SystemActionId::Calculator },
    { L"notepad",          SystemActionId::Notepad },
    { L"taskView",         SystemActionId::TaskView },
    { L"clipboardHistory", SystemActionId::ClipboardHistory },
    { L"emojiPanel",       SystemActionId::EmojiPanel },
    { L"lockScreen",       SystemActionId::LockScreen },
};
} // namespace

const wchar_t* SystemActionName(SystemActionId id) {
    for (const auto& e : kSystemActions) {
        if (e.id == id) return e.name;
    }
    return L"explorer";
}

bool SystemActionFromName(const std::wstring& name, SystemActionId& out) {
    const std::wstring k = ToLower(name);
    for (const auto& e : kSystemActions) {
        if (k == ToLower(e.name)) { out = e.id; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 默认配置
// ---------------------------------------------------------------------------

Config DefaultConfig() {
    Config c;
    c.action.type = ActionType::SendKey;
    c.action.vkey = VK_RCONTROL;
    return c;
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

std::string ConfigToJson(const Config& c) {
    json::Value root = json::Value::MakeObject();

    root.set("version",  json::Value(c.version));
    root.set("language", json::Value(WideToUtf8(c.language)));
    root.set("logLevel", json::Value(c.logLevel));
    root.set("enabled",  json::Value(c.enabled));

    json::Value mapping = json::Value::MakeObject();
    mapping.set("trigger",        json::Value(WideToUtf8(c.trigger)));
    mapping.set("reloadOnChange", json::Value(c.reloadOnChange));
    mapping.set("action",         ActionToJson(c.action));
    root.set("mapping", mapping);

    return json::serialize(root, 2);
}

// ---------------------------------------------------------------------------
// 反序列化
// ---------------------------------------------------------------------------

bool ConfigFromJson(const std::string& text, Config& out, std::wstring& error) {
    out = DefaultConfig();

    json::Value root;
    std::string parseError;
    if (!json::parse(text, root, parseError)) {
        error = L"JSON 语法错误: " + Utf8ToWide(parseError);
        return false;
    }
    if (!root.isObject()) {
        error = L"配置文件的顶层必须是一个 JSON 对象。";
        return false;
    }

    std::wstring warnings;

    const json::Value* version = root.find("version");
    if (version && version->isNumber()) out.version = version->asInt();

    out.language = GetWideString(root, "language", out.language);
    if (out.language.empty()) out.language = L"zh-CN";

    out.logLevel = root.find("logLevel") && root.at("logLevel").isString()
                       ? root.at("logLevel").asString()
                       : out.logLevel;

    out.enabled = GetBool(root, "enabled", out.enabled);

    if (const json::Value* mapping = root.find("mapping"); mapping && mapping->isObject()) {
        out.trigger        = GetWideString(*mapping, "trigger", out.trigger);
        out.reloadOnChange = GetBool(*mapping, "reloadOnChange", out.reloadOnChange);

        if (!EqualsIgnoreCase(out.trigger, L"copilotKey")) {
            warnings += L"暂不支持的触发方式 \"" + out.trigger + L"\"，仍按 Copilot 键处理。";
            out.trigger = L"copilotKey";
        }

        const json::Value* action = mapping->find("action");
        if (action) {
            ActionFromJson(*action, out.action, warnings);
        } else {
            warnings += L"配置中缺少 mapping.action，已使用默认动作（映射为右 Ctrl）。";
        }
    } else {
        warnings += L"配置中缺少 mapping 段，已使用默认动作（映射为右 Ctrl）。";
    }

    // 警告不算失败：配置能跑就先跑起来，问题记到日志里
    if (!warnings.empty()) {
        error = warnings;
    } else {
        error.clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// 文件读写
// ---------------------------------------------------------------------------

bool ConfigLoad(const std::wstring& path, Config& out, std::wstring& error) {
    std::string text;
    if (!ReadFileUtf8(path, text, error)) {
        out = DefaultConfig();
        return false;
    }
    return ConfigFromJson(text, out, error);
}

bool ConfigSave(const std::wstring& path, const Config& config, std::wstring& error) {
    return WriteFileUtf8(path, ConfigToJson(config), error);
}

// ---------------------------------------------------------------------------
// 变更监视
// ---------------------------------------------------------------------------

void ConfigWatcher::Watch(const std::wstring& path) {
    m_path = path;
    m_hasBaseline = false;
    m_lastWrite = 0;
    m_lastSize = 0;
}

bool ConfigWatcher::PollChanged() {
    if (m_path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }

    ULARGE_INTEGER writeTime;
    writeTime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    writeTime.LowPart  = fad.ftLastWriteTime.dwLowDateTime;

    ULARGE_INTEGER size;
    size.HighPart = fad.nFileSizeHigh;
    size.LowPart  = fad.nFileSizeLow;

    const bool changed = m_hasBaseline &&
                         (writeTime.QuadPart != m_lastWrite || size.QuadPart != m_lastSize);

    m_lastWrite = writeTime.QuadPart;
    m_lastSize  = size.QuadPart;
    m_hasBaseline = true;
    return changed;
}

// ---------------------------------------------------------------------------
// 路径与字符串
// ---------------------------------------------------------------------------

std::wstring GetModuleDirectory() {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0) return L".";

    std::wstring path(buffer, len);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring PathJoin(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring result = a;
    if (result.back() != L'\\' && result.back() != L'/') result += L'\\';
    result += b;
    return result;
}

std::wstring GetDefaultConfigPath() {
    return PathJoin(GetModuleDirectory(), kConfigFileName);
}

std::wstring GetDefaultLogPath() {
    return PathJoin(GetModuleDirectory(), kLogFileName);
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need);
    return out;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out, std::wstring& error) {
    out.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        wchar_t buf[64];
        std::swprintf(buf, 64, L"0x%08X", err);
        error = L"无法打开配置文件（错误码 " + std::wstring(buf) + L"）。";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(h);
        error = L"配置文件过大或无法读取大小（上限 8 MB）。";
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < out.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(out.size() - total);
        if (!ReadFile(h, out.data() + total, want, &read, nullptr) || read == 0) break;
        total += read;
    }
    CloseHandle(h);
    out.resize(total);

    // 去掉 UTF-8 BOM
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        wchar_t buf[64];
        std::swprintf(buf, 64, L"0x%08X", err);
        error = L"无法写入配置文件（错误码 " + std::wstring(buf) + L"）。";
        return false;
    }

    size_t total = 0;
    bool ok = true;
    while (total < text.size()) {
        DWORD written = 0;
        const DWORD want = static_cast<DWORD>(text.size() - total);
        if (!WriteFile(h, text.data() + total, want, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        total += written;
    }
    CloseHandle(h);

    if (!ok) error = L"写入配置文件时出错（磁盘已满或权限不足）。";
    return ok;
}

} // namespace rck
