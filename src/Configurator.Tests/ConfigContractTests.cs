using RemapCopilotKey.Configurator;

namespace RemapCopilotKey.Tests;

// ---------------------------------------------------------------------------
// 配置 JSON 契约测试（C# 侧）
//
// 这一层要守住的是"配置程序写出的 JSON"与"C++ 服务程序读入的 JSON"一致。
// 测试会额外把一份完整配置写到命令行传入的路径，
// 由 C++ 侧的 config_test.exe 再去解析同一个文件，形成跨语言闭环。
// ---------------------------------------------------------------------------

internal static class Program
{
    private static int s_pass;
    private static int s_fail;

    private static void Check(bool condition, string what)
    {
        if (condition)
        {
            s_pass++;
        }
        else
        {
            s_fail++;
            Console.WriteLine($"  [FAIL] {what}");
        }
    }

    private static void CheckEqual<T>(T actual, T expected, string what)
    {
        if (EqualityComparer<T>.Default.Equals(actual, expected))
        {
            s_pass++;
        }
        else
        {
            s_fail++;
            Console.WriteLine($"  [FAIL] {what}\n          实际: [{actual}]\n          期望: [{expected}]");
        }
    }

    private static int Main(string[] args)
    {
        Console.WriteLine("=== RemapCopilotKey 配置契约测试 (C#) ===\n");

        TestDefaultKeyNames();
        TestRoundTrip();
        TestActionTypeFields();
        TestValidation();
        TestKeyCatalog();
        TestLanguagePacks();
        TestJsonLayer();

        if (args.Length > 0)
        {
            WriteCrossLanguageSample(args[0]);
        }

        Console.WriteLine($"\n通过 {s_pass} 项，失败 {s_fail} 项");
        return s_fail == 0 ? 0 : 1;
    }

    // -----------------------------------------------------------------------

    private static void TestDefaultKeyNames()
    {
        Console.WriteLine("TestDefaultKeyNames");

        var json = new AppConfig().ToJson();
        var root = JsonParser.Parse(json);

        // 顶层字段名必须与 C++ 的 Config.cpp / config_test.cpp 完全一致
        foreach (var name in new[] { "version", "language", "logLevel", "enabled", "mapping" })
        {
            Check(root.TryGet(name, out _), $"顶层应包含字段 {name}");
        }

        // 服务已改为固定管理员权限、且只管配置，这两段都不应再出现
        Check(!root.TryGet("startup", out _), "不应再写出 startup 段（服务固定管理员）");
        Check(!root.TryGet("ui", out _), "不应再写出 ui 段（服务无托盘）");

        var mapping = root["mapping"];
        Check(mapping.TryGet("trigger", out var trigger), "mapping.trigger");
        CheckEqual(trigger.AsString(), "copilotKey", "mapping.trigger 默认值");
        Check(mapping.TryGet("reloadOnChange", out _), "mapping.reloadOnChange");

        var action = mapping["action"];
        Check(action.TryGet("type", out var type), "mapping.action.type");
        CheckEqual(type.AsString(), "sendKey", "默认动作类型");
        Check(action.TryGet("vkey", out var vkey), "mapping.action.vkey");
        CheckEqual(vkey.AsString(), "VK_RCONTROL", "默认目标按键必须是右 Ctrl");

        // 中文应该原样写成 UTF-8，而不是 \uXXXX
        var chinese = new AppConfig { Language = "zh-CN" }.ToJson();
        Check(chinese.IndexOf("\\u", StringComparison.Ordinal) < 0, "中文不应被转义成 \\uXXXX");

        var bytes = new System.Text.UTF8Encoding(false).GetBytes(json);
        Check(bytes.Length > 3 && !(bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF), "不应写 BOM");
    }

    private static void TestRoundTrip()
    {
        Console.WriteLine("TestRoundTrip");

        var original = new AppConfig
        {
            Version = 1,
            Language = "en-US",
            LogLevel = "debug",
            Enabled = false,
        };
        original.Mapping.ReloadOnChange = false;
        original.Mapping.Action = new ActionConfig
        {
            Type = ActionTypes.OpenApp,
            Path = @"C:\Windows\notepad.exe",
            Args = "/A",
            RunAsAdmin = true,
        };

        var restored = AppConfig.FromJson(original.ToJson());

        CheckEqual(restored.Language, original.Language, "language 往返");
        CheckEqual(restored.LogLevel, original.LogLevel, "logLevel 往返");
        CheckEqual(restored.Enabled, original.Enabled, "enabled 往返");
        CheckEqual(restored.Mapping.ReloadOnChange, original.Mapping.ReloadOnChange, "reloadOnChange 往返");
        CheckEqual(restored.Mapping.Action.Type, ActionTypes.OpenApp, "action.type 往返");
        CheckEqual(restored.Mapping.Action.Path, original.Mapping.Action.Path, "action.path 往返");
        CheckEqual(restored.Mapping.Action.Args, original.Mapping.Action.Args, "action.args 往返");
        CheckEqual(restored.Mapping.Action.RunAsAdmin, true, "action.runAsAdmin 往返");

        CheckEqual(restored.ToJson(), original.ToJson(), "序列化应幂等");
    }

    private static void TestActionTypeFields()
    {
        Console.WriteLine("TestActionTypeFields");

        // sendChord -> keys 数组（最多三个）
        var chord = new AppConfig();
        chord.Mapping.Action = new ActionConfig
        {
            Type = ActionTypes.SendChord,
            Keys = new List<string> { "VK_CONTROL", "VK_SHIFT", "VK_ESCAPE" },
        };
        {
            var action = JsonParser.Parse(chord.ToJson())["mapping"]["action"];
            Check(action.TryGet("keys", out var keys), "sendChord 应写出 keys");
            CheckEqual(keys.Count, 3, "keys 数量");
            CheckEqual(keys[0].AsString(), "VK_CONTROL", "keys[0]");
        }

        // 无关字段不应被写出（WhenWritingNull）
        {
            var action = JsonParser.Parse(chord.ToJson())["mapping"]["action"];
            Check(!action.TryGet("path", out _), "sendChord 不应写出 path");
            Check(!action.TryGet("url", out _), "sendChord 不应写出 url");
        }

        // openApp 的 runAsAdmin 必须显式写出（布尔值不能被 null 规则吞掉）
        var app = new AppConfig();
        app.Mapping.Action = new ActionConfig
        {
            Type = ActionTypes.OpenApp,
            Path = @"C:\Windows\System32\cmd.exe",
            RunAsAdmin = true,
            ToggleWindow = true,
        };
        {
            var action = JsonParser.Parse(app.ToJson())["mapping"]["action"];
            Check(action.TryGet("runAsAdmin", out var admin), "openApp 应写出 runAsAdmin");
            CheckEqual(admin.AsBool(), true, "runAsAdmin 值");
            Check(action.TryGet("toggleWindow", out var toggle), "openApp 应写出 toggleWindow");
            CheckEqual(toggle.AsBool(), true, "toggleWindow 值");
        }

        // openUrl -> url
        var url = new AppConfig();
        url.Mapping.Action = new ActionConfig { Type = ActionTypes.OpenUrl, Url = "https://example.com/x?y=1" };
        var restoredUrl = AppConfig.FromJson(url.ToJson());
        CheckEqual(restoredUrl.Mapping.Action.Url, "https://example.com/x?y=1", "openUrl 查询串不应被截断");

        // systemAction -> action
        var system = new AppConfig();
        system.Mapping.Action = new ActionConfig { Type = ActionTypes.SystemAction, Action = "taskManager" };
        {
            var action = JsonParser.Parse(system.ToJson())["mapping"]["action"];
            Check(action.TryGet("action", out var inner), "systemAction 应写出 action 字段");
            CheckEqual(inner.AsString(), "taskManager", "systemAction 值");
        }
    }

    private static void TestValidation()
    {
        Console.WriteLine("TestValidation");

        Check(new AppConfig().Validate().Count == 0, "默认配置应通过校验");

        var missingPath = new AppConfig();
        missingPath.Mapping.Action = new ActionConfig { Type = ActionTypes.OpenApp, Path = "  " };
        Check(missingPath.Validate().Count > 0, "openApp 缺 path 应报错");

        var emptyChord = new AppConfig();
        emptyChord.Mapping.Action = new ActionConfig { Type = ActionTypes.SendChord, Keys = new List<string>() };
        Check(emptyChord.Validate().Count > 0, "空组合键应报错");

        var tooLongChord = new AppConfig();
        tooLongChord.Mapping.Action = new ActionConfig
        {
            Type = ActionTypes.SendChord,
            Keys = new List<string> { "VK_A", "VK_B", "VK_C", "VK_D" },
        };
        Check(tooLongChord.Validate().Count > 0, "超过三个键的组合键应报错");

        var badType = new AppConfig();
        badType.Mapping.Action = new ActionConfig { Type = "nonsense" };
        Check(badType.Validate().Count > 0, "未知动作类型应报错");
    }

    private static void TestKeyCatalog()
    {
        Console.WriteLine("TestKeyCatalog");

        var names = KeyCatalog.All.Select(k => k.Name).ToList();
        Check(names.Count > 100, $"键表条目数应足够多（实际 {names.Count}）");
        CheckEqual(names.Distinct(StringComparer.OrdinalIgnoreCase).Count(), names.Count, "键名不应重复");

        var vks = KeyCatalog.All.Select(k => k.Vk).ToList();
        CheckEqual(vks.Distinct().Count(), vks.Count, "虚拟键码不应重复（输入捕获靠它反查）");


        foreach (var required in new[]
                 {
                     "VK_RCONTROL", "VK_LCONTROL", "VK_CONTROL",
                     "VK_LWIN", "VK_LSHIFT", "VK_F23",
                     "VK_SHIFT", "VK_MENU", "VK_ESCAPE", "VK_TAB",
                 })
        {
            Check(KeyCatalog.Find(required) is not null, $"键表应包含 {required}");
        }

        // 输入捕获依赖 虚拟键码 -> 规范名 的反查
        CheckEqual(KeyCatalog.FindByVirtualKey(0xA3)?.Name, "VK_RCONTROL", "0xA3 应反查到 VK_RCONTROL");
        CheckEqual(KeyCatalog.FindByVirtualKey(0x1B)?.Name, "VK_ESCAPE", "0x1B 应反查到 VK_ESCAPE");
        CheckEqual(KeyCatalog.FindByVirtualKey(0x11)?.Name, "VK_CONTROL", "0x11 应反查到 VK_CONTROL");
        CheckEqual(KeyCatalog.FindByVirtualKey(0x5B)?.Name, "VK_LWIN", "0x5B 应反查到 VK_LWIN");
        Check(KeyCatalog.FindByVirtualKey(0x00) is null, "0 不应反查到任何键");

        CheckEqual(KeyCatalog.DisplayName("VK_NOT_REAL"), "VK_NOT_REAL", "未知键名应原样显示");

        Check(SystemActionCatalog.All.Count >= 14, "实用功能条目数");
        foreach (var required in new[] { "explorer", "taskManager", "cmd", "powershell", "screenshot" })
        {
            Check(SystemActionCatalog.Find(required) is not null, $"实用功能应包含 {required}");
        }

        // 下拉框不能显示类型名（曾经因为漏设 FormattingEnabled 显示成 "SystemActionEntry { ... }"）
        foreach (var entry in SystemActionCatalog.All)
        {
            var text = SystemActionCatalog.Display(entry);
            Check(text.IndexOf("SystemActionEntry", StringComparison.Ordinal) < 0,
                  $"实用功能显示名不应包含类型名: {text}");
        }
    }

    private static void TestLanguagePacks()
    {
        Console.WriteLine("TestLanguagePacks");

        Check(Localization.Packs.Count >= 2, "至少应有两个语言包");

        var zh = Localization.Packs.FirstOrDefault(p => p.Code == "zh-CN");
        var en = Localization.Packs.FirstOrDefault(p => p.Code == "en-US");
        Check(zh is not null, "应包含 zh-CN 语言包");
        Check(en is not null, "应包含 en-US 语言包");

        if (zh is not null && en is not null)
        {
            var missingInEn = zh.Strings.Keys.Except(en.Strings.Keys, StringComparer.Ordinal).ToList();
            var missingInZh = en.Strings.Keys.Except(zh.Strings.Keys, StringComparer.Ordinal).ToList();
            CheckEqual(missingInEn.Count, 0, $"en-US 缺少的键: {string.Join(", ", missingInEn)}");
            CheckEqual(missingInZh.Count, 0, $"zh-CN 缺少的键: {string.Join(", ", missingInZh)}");
        }

        // 空框与捕获中的占位文字必须存在且确实被翻译过
        Localization.SetLanguage("zh-CN");
        Check(Localization.T("hotkey.empty") == "<空白>", "zh-CN 的 hotkey.empty");
        Check(Localization.T("hotkey.capturing") == "<按下键盘任意按键捕获>", "zh-CN 的 hotkey.capturing");

        var chinese = Localization.T("radio.sendKey");
        Localization.SetLanguage("en-US");
        var english = Localization.T("radio.sendKey");
        Check(chinese != english, "中英文的 radio.sendKey 文本应不同");
        Check(!english.StartsWith("radio.", StringComparison.Ordinal), "en-US 不应回退到键名");

        Localization.SetLanguage("zh-CN");
    }

    // -----------------------------------------------------------------------
    // 自带的极简 JSON 层（net48 没有 System.Text.Json，见 src/Configurator/Json.cs）
    //
    // 这一层替代了一个成熟库，所以语法覆盖必须自己守住：转义、UTF-8、代理对、
    // 手改配置时常见的注释与尾逗号、错误定位。
    // -----------------------------------------------------------------------
    private static void TestJsonLayer()
    {
        Console.WriteLine("TestJsonLayer");

        // --- 基本类型 -----------------------------------------------------
        CheckEqual(JsonParser.Parse("true").AsBool(), true, "true");
        CheckEqual(JsonParser.Parse("false").AsBool(true), false, "false");
        Check(JsonParser.Parse("null").IsNull, "null");
        CheckEqual(JsonParser.Parse("42").AsInt(), 42, "整数");
        CheckEqual(JsonParser.Parse("-7").AsInt(), -7, "负数");
        CheckEqual(JsonParser.Parse("3.5").AsDouble(), 3.5, "小数");
        CheckEqual(JsonParser.Parse("1e3").AsInt(), 1000, "科学计数法");
        CheckEqual(JsonParser.Parse("\"hi\"").AsString(), "hi", "字符串");

        // --- 对象与数组 ---------------------------------------------------
        var value = JsonParser.Parse("""{"a":{"b":[1,2,3]},"c":[]}""");
        Check(value.IsObject, "根应为对象");
        CheckEqual(value["a"]["b"].Count, 3, "嵌套数组长度");
        CheckEqual(value["a"]["b"][1].AsInt(), 2, "嵌套数组元素");
        CheckEqual(value["c"].Count, 0, "空数组");
        CheckEqual(value["a"]["b"][99].IsNull, true, "越界索引返回 null");
        CheckEqual(value["nope"].IsNull, true, "缺失成员返回 null");

        // --- 缩进写出 -----------------------------------------------------
        var written = JsonWriter.Write(JsonParser.Parse("""{"x":[1,2]}"""));
        CheckEqual(written, "{\n  \"x\": [\n    1,\n    2\n  ]\n}", "缩进格式（两个空格）");
        CheckEqual(JsonWriter.Write(JsonParser.Parse("{}")), "{}", "空对象");
        CheckEqual(JsonWriter.Write(JsonParser.Parse("[]")), "[]", "空数组");
        CheckEqual(JsonWriter.Write(JsonParser.Parse("1.0")), "1", "整数值不写小数点");

        // --- 转义 ---------------------------------------------------------
        CheckEqual(JsonParser.Parse("\"a\\nb\"").AsString(), "a\nb", "\\n 转义");
        CheckEqual(JsonParser.Parse("\"\\u4e2d\\u6587\"").AsString(), "中文", "\\uXXXX 转义");
        CheckEqual(JsonParser.Parse("\"\\ud83d\\ude00\"").AsString(), "\U0001F600", "代理对合成一个字符");
        CheckEqual(JsonParser.Parse("\"a\\\\b\\\"c\"").AsString(), "a\\b\"c", "反斜杠与引号转义");
        CheckEqual(JsonParser.Parse("\"a\\/b\"").AsString(), "a/b", "\\/ 转义");

        // 写出的字符串必须能被自己读回来（含控制字符）
        var roundTripText = "换行\n制表\t引号\"反斜杠\\中文😀";
        var escaped = new System.Text.StringBuilder();
        JsonWriter.WriteString(escaped, roundTripText);
        CheckEqual(JsonParser.Parse(escaped.ToString()).AsString(), roundTripText, "字符串转义往返");
        Check(escaped.ToString().IndexOf("\\u4e2d", StringComparison.Ordinal) < 0,
              "非 ASCII 不应被转义成 \\uXXXX");

        // --- 手改配置的容错 ------------------------------------------------
        CheckEqual(JsonParser.Parse("{ // 注释\n \"a\": 1, }")["a"].AsInt(), 1, "行注释与尾逗号");
        CheckEqual(JsonParser.Parse("/* 块\n注释 */ {\"a\":1}")["a"].AsInt(), 1, "块注释");
        CheckEqual(JsonParser.Parse("{\"a\":[1,2,],}")["a"].Count, 2, "数组尾逗号");

        // --- 错误定位 -----------------------------------------------------
        CheckThrowsJson("{\"a\":}", "缺值");
        CheckThrowsJson("{\"a\" 1}", "缺冒号");
        CheckThrowsJson("{\"a\":1", "对象未闭合");
        CheckThrowsJson("[1,2", "数组未闭合");
        CheckThrowsJson("\"abc", "字符串未闭合");
        CheckThrowsJson("{} extra", "文档后有多余内容");
        CheckThrowsJson("{\"a\":1}{}", "两个文档");
    }

    private static void CheckThrowsJson(string json, string what)
    {
        try
        {
            JsonParser.Parse(json);
            Check(false, $"{what} 应当解析失败");
        }
        catch (JsonParseException)
        {
            Check(true, $"{what} 解析失败并抛出 JsonParseException");
        }
    }

    // -----------------------------------------------------------------------
    // 写出一份"完整"配置，交给 C++ 侧的 config_test.exe 解析
    // -----------------------------------------------------------------------
    private static void WriteCrossLanguageSample(string path)
    {
        Console.WriteLine($"WriteCrossLanguageSample -> {path}");

        var config = new AppConfig
        {
            Version = 1,
            Language = "en-US",
            LogLevel = "debug",
            Enabled = false,
        };
        config.Mapping.ReloadOnChange = false;
        config.Mapping.Action = new ActionConfig
        {
            Type = ActionTypes.SendChord,
            Keys = new List<string> { "VK_CONTROL", "VK_SHIFT", "VK_ESCAPE" },
        };

        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

        config.Save(path);
        Check(File.Exists(path), "跨语言样例配置应已写出");
    }
}
