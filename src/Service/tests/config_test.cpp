// ---------------------------------------------------------------------------
// RemapCopilotKey - 配置与键名模块单元测试
//
// 这一层是 C++ 服务程序与 C# 配置程序之间的接口契约，
// 所以重点验证：配置程序写出来的 JSON，服务程序必须能原样读回。
// ---------------------------------------------------------------------------

#include "../src/Config.h"
#include "../src/Keys.h"
#include "../src/Log.h"

#include <cstdio>
#include <string>

using namespace rck;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) { ++g_pass; }                                                   \
        else {                                                                    \
            ++g_fail;                                                             \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                         \
    } while (0)

#define CHECK_WSTR(a, b)                                                         \
    do {                                                                         \
        const std::wstring _a = (a);                                              \
        const std::wstring _b = (b);                                              \
        if (_a == _b) { ++g_pass; }                                               \
        else {                                                                    \
            ++g_fail;                                                             \
            std::printf("  [FAIL] %s:%d\n          实际: [%ls]\n          期望: [%ls]\n", \
                        __FILE__, __LINE__, _a.c_str(), _b.c_str());              \
        }                                                                         \
    } while (0)

#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        const std::string _a = (a);                                               \
        const std::string _b = (b);                                               \
        if (_a == _b) { ++g_pass; }                                               \
        else {                                                                    \
            ++g_fail;                                                             \
            std::printf("  [FAIL] %s:%d\n          实际: [%s]\n          期望: [%s]\n", \
                        __FILE__, __LINE__, _a.c_str(), _b.c_str());              \
        }                                                                         \
    } while (0)

// ---------------------------------------------------------------------------

static void TestKeyNames() {
    std::printf("TestKeyNames\n");

    CHECK(VKeyFromName(L"VK_RCONTROL") == VK_RCONTROL);
    CHECK(VKeyFromName(L"vk_rcontrol") == VK_RCONTROL);   // 大小写不敏感
    CHECK(VKeyFromName(L"RCONTROL") == VK_RCONTROL);      // 允许省略 VK_ 前缀
    CHECK(VKeyFromName(L"RCTRL") == VK_RCONTROL);         // 别名
    // 通用修饰键必须解析成 VK_CONTROL / VK_SHIFT，
    // 不能被右键别名遮蔽（否则组合键会走偏）
    CHECK(VKeyFromName(L"VK_CONTROL") == VK_CONTROL);
    CHECK(VKeyFromName(L"VK_SHIFT") == VK_SHIFT);
    CHECK(VKeyFromName(L"VK_MENU") == VK_MENU);
    CHECK(VKeyFromName(L"CTRL") == VK_CONTROL);
    CHECK(VKeyFromName(L"CONTROL") == VK_CONTROL);
    CHECK(VKeyFromName(L"SHIFT") == VK_SHIFT);
    CHECK(VKeyFromName(L"ALT") == VK_MENU);
    CHECK(VKeyFromName(L"A") == L'A');
    CHECK(VKeyFromName(L"F23") == VK_F23);
    CHECK(VKeyFromName(L"VK_F23") == VK_F23);
    CHECK(VKeyFromName(L"VK_LWIN") == VK_LWIN);
    CHECK(VKeyFromName(L"VK_OEM_PERIOD") == VK_OEM_PERIOD);
    CHECK(VKeyFromName(L"7") == L'7');

    // 非法输入必须返回 0，而不是某个碰巧有效的键码
    CHECK(VKeyFromName(L"") == 0);
    CHECK(VKeyFromName(L"VK_NOT_A_REAL_KEY") == 0);
    CHECK(VKeyFromName(L"@@@") == 0);

    // 反查
    CHECK_WSTR(VKeyName(VK_RCONTROL), L"VK_RCONTROL");
    CHECK_WSTR(VKeyName(VK_F23), L"VK_F23");
    CHECK_WSTR(VKeyName(L'A'), L"VK_A");
    CHECK_WSTR(VKeyName(0xFE), L"VK_OEM_CLEAR");
    // 未知键码给一个可诊断的形式，而不是空字符串
    CHECK(VKeyName(0x00)[0] == L'V');

    // 键表不能有重复的规范名或重复的键码
    const auto& keys = AllKeys();
    CHECK(keys.size() > 100);
    for (size_t i = 0; i < keys.size(); ++i) {
        for (size_t j = i + 1; j < keys.size(); ++j) {
            if (EqualsIgnoreCase(keys[i].name, keys[j].name)) {
                ++g_fail;
                std::printf("  [FAIL] 键名重复: %ls\n", keys[i].name);
            }
            if (keys[i].vkey == keys[j].vkey) {
                ++g_fail;
                std::printf("  [FAIL] 键码重复: %ls 与 %ls 都是 0x%02X\n",
                            keys[i].name, keys[j].name, keys[i].vkey);
            }
        }
    }
    ++g_pass;

    // 每个已知键名都必须能解析回自己的键码（往返一致）
    for (const KeyInfo& k : keys) {
        if (VKeyFromName(k.name) != k.vkey) {
            ++g_fail;
            std::printf("  [FAIL] 往返失败: %ls\n", k.name);
        }
    }
    ++g_pass;
}

static void TestDefaultRoundTrip() {
    std::printf("TestDefaultRoundTrip\n");

    const Config original = DefaultConfig();
    const std::string text = ConfigToJson(original);

    // 默认动作必须是右 Ctrl
    CHECK(original.action.type == ActionType::SendKey);
    CHECK(original.action.vkey == VK_RCONTROL);

    Config parsed;
    std::wstring error;
    CHECK(ConfigFromJson(text, parsed, error));
    CHECK(error.empty());

    CHECK(parsed.version == original.version);
    CHECK_WSTR(parsed.language, original.language);
    CHECK_STR(parsed.logLevel, original.logLevel);
    CHECK(parsed.enabled == original.enabled);

    CHECK(parsed.reloadOnChange == original.reloadOnChange);
    CHECK_WSTR(parsed.trigger, L"copilotKey");
    CHECK(parsed.action.type == ActionType::SendKey);
    CHECK(parsed.action.vkey == VK_RCONTROL);

    // 二次序列化必须字节级一致（幂等）
    CHECK_STR(ConfigToJson(parsed), text);
}

// 这段 JSON 就是 C# 配置程序将要写出的格式。
// 一旦两边对不上，这个测试会立刻失败。
static void TestConfiguratorFormat() {
    std::printf("TestConfiguratorFormat\n");

    const char* kJson = R"({
  "version": 1,
  "language": "en-US",
  "logLevel": "debug",
  "enabled": false,

  "mapping": {
    "trigger": "copilotKey",
    "reloadOnChange": false,
    "action": {
      "type": "sendChord",
      "keys": [ "VK_CONTROL", "VK_SHIFT", "VK_ESCAPE" ]
    }
  }
})";

    Config c;
    std::wstring error;
    CHECK(ConfigFromJson(kJson, c, error));
    CHECK(error.empty());

    CHECK_WSTR(c.language, L"en-US");
    CHECK_STR(c.logLevel, "debug");
    CHECK(c.enabled == false);
    CHECK(c.reloadOnChange == false);
    CHECK(c.action.type == ActionType::SendChord);
    CHECK(c.action.chord.size() == 3);
    CHECK(c.action.chord[0] == VK_CONTROL);
    CHECK(c.action.chord[1] == VK_SHIFT);
    CHECK(c.action.chord[2] == VK_ESCAPE);
}

static void TestAllActionTypes() {
    std::printf("TestAllActionTypes\n");

    struct Case {
        const char*  json;
        ActionType   type;
    };
    const Case cases[] = {
        { R"({"mapping":{"action":{"type":"sendKey","vkey":"VK_RCONTROL"}}})", ActionType::SendKey },
        { R"({"mapping":{"action":{"type":"sendChord","keys":["VK_LWIN","VK_TAB"]}}})", ActionType::SendChord },
        { R"({"mapping":{"action":{"type":"openApp","path":"C:\\Windows\\notepad.exe","args":"/A"}}})", ActionType::OpenApp },
        { R"({"mapping":{"action":{"type":"openUrl","url":"https://example.com/x?y=1"}}})", ActionType::OpenUrl },
        { R"({"mapping":{"action":{"type":"systemAction","action":"taskManager"}}})", ActionType::SystemAction },
    };

    for (const Case& c : cases) {
        Config cfg;
        std::wstring error;
        const bool ok = ConfigFromJson(c.json, cfg, error);
        CHECK(ok);
        CHECK(cfg.action.type == c.type);
        if (!ok) std::printf("      解析失败: %ls\n", error.c_str());
    }

    // openApp 的路径与参数要完整保留
    {
        Config cfg;
        std::wstring error;
        ConfigFromJson(cases[2].json, cfg, error);
        CHECK_WSTR(cfg.action.path, L"C:\\Windows\\notepad.exe");
        CHECK_WSTR(cfg.action.arguments, L"/A");
        // 没写这两个开关时默认为 false
        CHECK(cfg.action.runAsAdmin == false);
        CHECK(cfg.action.toggleWindow == false);
    }
    // openApp 的 runAsAdmin / toggleWindow 开关
    {
        Config cfg;
        std::wstring error;
        ConfigFromJson(
            R"({"mapping":{"action":{"type":"openApp","path":"C:\\Windows\\System32\\cmd.exe","runAsAdmin":true,"toggleWindow":true}}})",
            cfg, error);
        CHECK(cfg.action.runAsAdmin == true);
        CHECK(cfg.action.toggleWindow == true);

        // 往返一次不能丢
        Config again;
        ConfigFromJson(ConfigToJson(cfg), again, error);
        CHECK(again.action.runAsAdmin == true);
        CHECK(again.action.toggleWindow == true);
    }
    // openUrl 里的查询串不能被截断
    {
        Config cfg;
        std::wstring error;
        ConfigFromJson(cases[3].json, cfg, error);
        CHECK_WSTR(cfg.action.url, L"https://example.com/x?y=1");
    }
    // systemAction 要正确映射到枚举
    {
        Config cfg;
        std::wstring error;
        ConfigFromJson(cases[4].json, cfg, error);
        CHECK(cfg.action.systemAction == SystemActionId::TaskManager);
    }
}

static void TestWarningsAndFallbacks() {
    std::printf("TestWarningsAndFallbacks\n");

    // 未知动作类型 -> 回退到默认动作，并给出警告
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(R"({"mapping":{"action":{"type":"noSuchType"}}})", c, error));
        CHECK(c.action.type == ActionType::SendKey);
        CHECK(c.action.vkey == VK_RCONTROL);
        CHECK(!error.empty());
    }

    // 未知按键名 -> 回退到右 Ctrl，并给出警告
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(R"({"mapping":{"action":{"type":"sendKey","vkey":"VK_BOGUS"}}})", c, error));
        CHECK(c.action.vkey == VK_RCONTROL);
        CHECK(!error.empty());
    }

    // 空组合键 -> 回退到 Ctrl+Shift+Esc
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(R"({"mapping":{"action":{"type":"sendChord","keys":[]}}})", c, error));
        CHECK(c.action.chord.size() == 3);
        CHECK(c.action.chord[2] == VK_ESCAPE);
        CHECK(!error.empty());
    }

    // 组合键里混入非法项 -> 忽略该项，保留合法项
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(
            R"({"mapping":{"action":{"type":"sendChord","keys":["VK_LWIN","VK_BOGUS","VK_TAB"]}}})",
            c, error));
        CHECK(c.action.chord.size() == 2);
        CHECK(c.action.chord[0] == VK_LWIN);
        CHECK(c.action.chord[1] == VK_TAB);
        CHECK(!error.empty());
    }

    // 不支持的触发器 -> 回退到 Copilot 键
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(R"({"mapping":{"trigger":"someOtherKey"}})", c, error));
        CHECK_WSTR(c.trigger, L"copilotKey");
        CHECK(!error.empty());
    }

    // 未知字段应被忽略，而不是报错
    {
        Config c;
        std::wstring error;
        CHECK(ConfigFromJson(R"({"futureField":{"nested":[1,2,3]},"enabled":true})", c, error));
        CHECK(c.enabled == true);
    }

    // JSON 语法错误 -> 失败，而不是崩溃
    {
        Config c;
        std::wstring error;
        CHECK(!ConfigFromJson("{ not json ", c, error));
        CHECK(!error.empty());
        // 失败时也要给出可用的默认配置
        CHECK(c.action.type == ActionType::SendKey);
        CHECK(c.action.vkey == VK_RCONTROL);
    }

    // 顶层不是对象 -> 失败
    {
        Config c;
        std::wstring error;
        CHECK(!ConfigFromJson("[1,2,3]", c, error));
    }
}

static void TestPathsAndEncoding() {
    std::printf("TestPathsAndEncoding\n");

    CHECK_WSTR(PathJoin(L"C:\\a", L"b.json"), L"C:\\a\\b.json");
    CHECK_WSTR(PathJoin(L"C:\\a\\", L"b.json"), L"C:\\a\\b.json");
    CHECK_WSTR(PathJoin(L"", L"b.json"), L"b.json");

    // UTF-8 往返（中文路径、中文值）
    const std::wstring chinese = L"中文路径\\配置文件.json";
    CHECK_WSTR(Utf8ToWide(WideToUtf8(chinese)), chinese);

    // 含中文的配置应能往返
    Config c = DefaultConfig();
    c.language = L"zh-CN";
    c.action.type = ActionType::OpenUrl;
    c.action.url = L"https://例子.测试/路径?q=中文";
    const std::string text = ConfigToJson(c);

    Config back;
    std::wstring error;
    CHECK(ConfigFromJson(text, back, error));
    CHECK_WSTR(back.action.url, c.action.url);
    // 中文应保持为可直接阅读的 UTF-8，而不是 \uXXXX
    CHECK(text.find("\\u") == std::string::npos);

    // 模块目录必须存在（测试进程自己的目录）
    const std::wstring dir = GetModuleDirectory();
    CHECK(!dir.empty());
    CHECK(GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES);
}

static void TestSystemActionNames() {
    std::printf("TestSystemActionNames\n");

    const SystemActionId ids[] = {
        SystemActionId::Explorer, SystemActionId::TaskManager, SystemActionId::Cmd,
        SystemActionId::PowerShell, SystemActionId::Screenshot, SystemActionId::SnippingTool,
        SystemActionId::Settings, SystemActionId::ControlPanel, SystemActionId::Calculator,
        SystemActionId::Notepad, SystemActionId::TaskView, SystemActionId::ClipboardHistory,
        SystemActionId::EmojiPanel, SystemActionId::LockScreen,
    };

    for (SystemActionId id : ids) {
        const std::wstring name = SystemActionName(id);
        CHECK(!name.empty());
        SystemActionId back = SystemActionId::Explorer;
        CHECK(SystemActionFromName(name, back));
        CHECK(back == id);
    }

    SystemActionId dummy = SystemActionId::Explorer;
    CHECK(!SystemActionFromName(L"nope", dummy));
    CHECK(SystemActionFromName(L"TASKMANAGER", dummy));   // 大小写不敏感
    CHECK(dummy == SystemActionId::TaskManager);
}

// 跨语言契约测试：解析由 C# 配置程序写出的真实文件。
// 由 build.ps1 在跑完 C# 侧的契约测试后把路径传进来。
static void TestExternalConfigFile(const std::wstring& path) {
    std::printf("TestExternalConfigFile\n");
    std::printf("  文件: %ls\n", path.c_str());

    Config c;
    std::wstring error;
    const bool ok = ConfigLoad(path, c, error);

    CHECK(ok);
    if (!ok) {
        std::printf("      读取失败: %ls\n", error.c_str());
        return;
    }

    // 这些期望值来自 Configurator.Tests 里写出的样例配置，
    // 两边必须严格对应；任何一侧改了格式，这里会立刻失败。
    CHECK_WSTR(c.language, L"en-US");
    CHECK_STR(c.logLevel, "debug");
    CHECK(c.enabled == false);
    CHECK(c.reloadOnChange == false);
    CHECK_WSTR(c.trigger, L"copilotKey");

    CHECK(c.action.type == ActionType::SendChord);
    CHECK(c.action.chord.size() == 3);
    if (c.action.chord.size() == 3) {
        CHECK(c.action.chord[0] == VK_CONTROL);
        CHECK(c.action.chord[1] == VK_SHIFT);
        CHECK(c.action.chord[2] == VK_ESCAPE);
    }

    // 解析真实的 C# 输出不应产生任何警告
    CHECK(error.empty());
    if (!error.empty()) {
        std::printf("      意外警告: %ls\n", error.c_str());
    }
}

int main(int argc, char** argv) {
    std::printf("=== RemapCopilotKey 配置模块测试 ===\n\n");

    TestKeyNames();
    TestDefaultRoundTrip();
    TestConfiguratorFormat();
    TestAllActionTypes();
    TestWarningsAndFallbacks();
    TestPathsAndEncoding();
    TestSystemActionNames();

    if (argc > 1) {
        TestExternalConfigFile(Utf8ToWide(argv[1]));
    } else {
        std::printf("TestExternalConfigFile\n  (跳过：未提供外部配置文件路径)\n");
    }

    std::printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
