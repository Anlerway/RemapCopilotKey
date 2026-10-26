using System.Reflection;
using System.Text;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 多语言支持
//
// 语言包是嵌入资源（Lang\*.json），新增语言只需要往 Lang\ 里放一个 JSON，
// 不需要改任何代码。界面刷新通过 LanguageChanged 事件广播。
//
// JSON 由本项目的 Json.cs 解析（net48 没有 System.Text.Json）。
// ---------------------------------------------------------------------------

public sealed class LanguagePack
{
    public string Code { get; init; } = "en-US";
    public string Name { get; init; } = "English";
    public Dictionary<string, string> Strings { get; init; } = new();
}

public static class Localization
{
    private static readonly List<LanguagePack> s_packs = LoadPacks();
    private static LanguagePack s_current = s_packs.FirstOrDefault(p => p.Code == "zh-CN")
                                            ?? s_packs.FirstOrDefault()
                                            ?? new LanguagePack();

    /// <summary>界面语言变化时触发，主窗体据此刷新所有文本。</summary>
    public static event Action? LanguageChanged;

    public static IReadOnlyList<LanguagePack> Packs => s_packs;

    public static string CurrentCode => s_current.Code;

    public static bool IsChinese => s_current.Code.StartsWith("zh", StringComparison.OrdinalIgnoreCase);

    public static void SetLanguage(string code)
    {
        var pack = s_packs.FirstOrDefault(p => string.Equals(p.Code, code, StringComparison.OrdinalIgnoreCase));
        if (pack is null || ReferenceEquals(pack, s_current)) return;

        s_current = pack;
        LanguageChanged?.Invoke();
    }

    /// <summary>取本地化文本；找不到键时退回键名本身，便于发现漏翻。</summary>
    public static string T(string key)
    {
        return s_current.Strings.TryGetValue(key, out var value) ? value : key;
    }

    /// <summary>带格式参数的版本，例如 T("status.autoSaved", "14:23:07")。</summary>
    public static string T(string key, params object?[] args)
        => string.Format(T(key), args);

    private static List<LanguagePack> LoadPacks()
    {
        var result = new List<LanguagePack>();
        var assembly = Assembly.GetExecutingAssembly();

        foreach (var resourceName in assembly.GetManifestResourceNames())
        {
            if (!resourceName.EndsWith(".json", StringComparison.OrdinalIgnoreCase)) continue;
            // net48 没有 string.Contains(string, StringComparison)，用 IndexOf 代替
            if (resourceName.IndexOf(".Lang.", StringComparison.OrdinalIgnoreCase) < 0) continue;

            try
            {
                using var stream = assembly.GetManifestResourceStream(resourceName);
                if (stream is null) continue;

                using var reader = new StreamReader(stream, System.Text.Encoding.UTF8);
                var pack = ParsePack(reader.ReadToEnd());

                if (pack is not null && !string.IsNullOrWhiteSpace(pack.Code))
                {
                    result.Add(pack);
                }
            }
            catch (JsonParseException)
            {
                // 单个语言包损坏不应该让整个程序起不来
            }
        }

        // 中文优先，其余按代码排序，保证顺序稳定
        result.Sort((a, b) =>
        {
            var aChinese = a.Code.StartsWith("zh", StringComparison.OrdinalIgnoreCase);
            var bChinese = b.Code.StartsWith("zh", StringComparison.OrdinalIgnoreCase);
            if (aChinese != bChinese) return aChinese ? -1 : 1;
            return string.Compare(a.Code, b.Code, StringComparison.OrdinalIgnoreCase);
        });

        if (result.Count == 0)
        {
            result.Add(new LanguagePack
            {
                Code = "en-US",
                Name = "English",
                Strings = new Dictionary<string, string>(),
            });
        }

        return result;
    }

    private static LanguagePack? ParsePack(string json)
    {
        var root = JsonParser.Parse(json);
        if (!root.IsObject) return null;

        var strings = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var key in root["strings"].Names)
        {
            if (root["strings"][key].AsString() is { } value) strings[key] = value;
        }

        return new LanguagePack
        {
            Code = root["code"].AsString() ?? string.Empty,
            Name = root["name"].AsString() ?? string.Empty,
            Strings = strings,
        };
    }
}
