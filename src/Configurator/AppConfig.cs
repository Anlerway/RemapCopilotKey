namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 配置模型
//
// 字段名与 C++ 服务程序的 Config.h / Config.cpp 一一对应。
// 序列化时用 camelCase + 忽略 null，写出来的 JSON 必须能被服务程序原样读回；
// 这一点由 src/Service/tests/config_test.cpp 里的 TestConfiguratorFormat 守住。
//
// JSON 由本项目的 Json.cs 读写（net48 没有 System.Text.Json，
// 见该文件头部注释说明为什么自己实现而不是引 NuGet 包）。
// ---------------------------------------------------------------------------

/// <summary>动作类型（与 C++ 的 ActionTypeName 保持一致）。</summary>
public static class ActionTypes
{
    public const string SendKey = "sendKey";
    public const string SendChord = "sendChord";
    public const string OpenApp = "openApp";
    public const string OpenUrl = "openUrl";
    public const string SystemAction = "systemAction";
}

public sealed class ActionConfig
{
    public string Type { get; set; } = ActionTypes.SendKey;

    /// <summary>sendKey：目标按键的规范名，例如 "VK_RCONTROL"。</summary>
    public string? Vkey { get; set; } = "VK_RCONTROL";

    /// <summary>sendChord：按下顺序排列的按键规范名。</summary>
    public List<string>? Keys { get; set; }

    /// <summary>openApp：可执行文件路径。</summary>
    public string? Path { get; set; }

    /// <summary>openApp：命令行参数。</summary>
    public string? Args { get; set; }

    /// <summary>openApp：被打开的程序是否以管理员权限运行（ShellExecute 的 runas 动词）。</summary>
    public bool RunAsAdmin { get; set; }

    /// <summary>
    /// openApp：程序已在运行时改为最小化/还原它的窗口，而不是再启动一个实例。
    /// </summary>
    public bool ToggleWindow { get; set; }

    /// <summary>openUrl：网址。</summary>
    public string? Url { get; set; }

    /// <summary>systemAction：实用功能标识，例如 "taskManager"。</summary>
    public string? Action { get; set; }
}

public sealed class MappingConfig
{
    /// <summary>触发方式。目前只支持 Copilot 键。</summary>
    public string Trigger { get; set; } = "copilotKey";

    public bool ReloadOnChange { get; set; } = true;

    public ActionConfig Action { get; set; } = new();
}

public sealed class AppConfig
{
    public int Version { get; set; } = 1;

    public string Language { get; set; } = "zh-CN";

    /// <summary>日志级别。默认只记错误，避免正常使用时被日志淹没。</summary>
    public string LogLevel { get; set; } = "error";

    public bool Enabled { get; set; } = true;

    public MappingConfig Mapping { get; set; } = new();

    // -- 读写 ---------------------------------------------------------------

    public string ToJson() => JsonWriter.Write(ToJsonValue());

    /// <summary>转成 JSON 值树。字段顺序与手写示例保持一致，便于人工比对。</summary>
    public JsonValue ToJsonValue()
    {
        var action = new JsonObject().Set("type", Mapping.Action.Type);

        switch (Mapping.Action.Type)
        {
            case ActionTypes.SendChord:
                var keys = new JsonArray();
                foreach (var key in Mapping.Action.Keys ?? new List<string>()) keys.Add(key);
                action.Set("keys", keys);
                break;

            case ActionTypes.SystemAction:
                action.Set("action", Mapping.Action.Action);
                break;

            case ActionTypes.OpenApp:
                action.Set("path", Mapping.Action.Path);
                action.Set("args", Mapping.Action.Args);
                action.Set("runAsAdmin", Mapping.Action.RunAsAdmin);
                action.Set("toggleWindow", Mapping.Action.ToggleWindow);
                break;

            case ActionTypes.OpenUrl:
                action.Set("url", Mapping.Action.Url);
                break;

            default:
                action.Set("vkey", Mapping.Action.Vkey);
                break;
        }

        var mapping = new JsonObject()
            .Set("trigger", Mapping.Trigger)
            .Set("reloadOnChange", Mapping.ReloadOnChange)
            .Set("action", JsonValue.Of(action));

        return JsonValue.Of(new JsonObject()
            .Set("version", Version)
            .Set("language", Language)
            .Set("logLevel", LogLevel)
            .Set("enabled", Enabled)
            .Set("mapping", JsonValue.Of(mapping)));
    }

    public static AppConfig FromJson(string json) => FromJsonValue(JsonParser.Parse(json));

    public static AppConfig FromJsonValue(JsonValue root)
    {
        if (!root.IsObject) throw new JsonParseException("配置文件的根应该是一个 JSON 对象");

        var config = new AppConfig
        {
            Version = root["version"].AsInt(1),
            Language = root["language"].AsString() ?? "zh-CN",
            LogLevel = root["logLevel"].AsString() ?? "error",
            Enabled = root["enabled"].AsBool(true),
        };

        var mapping = root["mapping"];
        if (mapping.IsObject)
        {
            config.Mapping.Trigger = mapping["trigger"].AsString() ?? "copilotKey";
            config.Mapping.ReloadOnChange = mapping["reloadOnChange"].AsBool(true);
        }

        var action = mapping["action"];
        if (action.IsObject)
        {
            var model = new ActionConfig
            {
                Type = action["type"].AsString() ?? ActionTypes.SendKey,
                Vkey = action["vkey"].AsString(),
                Path = action["path"].AsString(),
                Args = action["args"].AsString(),
                RunAsAdmin = action["runAsAdmin"].AsBool(),
                ToggleWindow = action["toggleWindow"].AsBool(),
                Url = action["url"].AsString(),
                Action = action["action"].AsString(),
            };

            var keys = action["keys"];
            if (keys.IsArray)
            {
                model.Keys = new List<string>();
                foreach (var key in keys.Items)
                {
                    if (key.AsString() is { } name) model.Keys.Add(name);
                }
            }

            // 只有 sendKey 才写出 vkey；其余类型读回来时补上界面需要的默认值
            if (model.Type == ActionTypes.SendKey && string.IsNullOrWhiteSpace(model.Vkey))
            {
                model.Vkey = "VK_RCONTROL";
            }

            config.Mapping.Action = model;
        }

        return config;
    }

    public static AppConfig Load(string path)
    {
        if (!File.Exists(path)) return new AppConfig();
        var text = File.ReadAllText(path, System.Text.Encoding.UTF8);
        return FromJson(text);
    }

    public void Save(string path)
    {
        // 与 C++ 侧一致：UTF-8 无 BOM
        File.WriteAllText(path, ToJson(), new System.Text.UTF8Encoding(false));
    }

    /// <summary>做一次合理性检查，返回人类可读的问题列表（空表示没问题）。</summary>
    public IReadOnlyList<string> Validate()
    {
        var problems = new List<string>();

        switch (Mapping.Action.Type)
        {
            case ActionTypes.SendKey:
                if (string.IsNullOrWhiteSpace(Mapping.Action.Vkey))
                    problems.Add("sendKey.vkey");
                break;

            case ActionTypes.SendChord:
                if (Mapping.Action.Keys is null || Mapping.Action.Keys.Count == 0)
                    problems.Add("sendChord.keys");
                else if (Mapping.Action.Keys.Count > 3)
                    problems.Add("sendChord.keys(最多三个)");
                break;

            case ActionTypes.OpenApp:
                if (string.IsNullOrWhiteSpace(Mapping.Action.Path))
                    problems.Add("openApp.path");
                break;

            case ActionTypes.OpenUrl:
                if (string.IsNullOrWhiteSpace(Mapping.Action.Url))
                    problems.Add("openUrl.url");
                break;

            case ActionTypes.SystemAction:
                if (string.IsNullOrWhiteSpace(Mapping.Action.Action))
                    problems.Add("systemAction.action");
                break;

            default:
                problems.Add("action.type");
                break;
        }

        return problems;
    }
}
