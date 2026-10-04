using System.Globalization;
using System.Text;

namespace RemapCopilotKey.Configurator;

// ---------------------------------------------------------------------------
// 极简 JSON 层（net48 用，替换 System.Text.Json）
//
// 为什么自己写：
//   net48 不含 System.Text.Json，用它就得引入 NuGet 包 —— 而一个自包含的
//   JSON 解析器只有两百来行，本项目的 C++ 侧（src/Service/src/Json.cpp）也是
//   自己实现的。这样 C# 侧同样做到**零第三方依赖、零 NuGet restore**。
//
// 写入格式刻意与原来的 System.Text.Json 配置保持一致：
//   * 缩进两个空格，对象/数组逐行展开（WriteIndented）
//   * 非 ASCII 原样输出，不写成 \uXXXX
//   * 字符串值里只转义 " \ 和控制字符
//   * 空对象写 {}、空数组写 []
//
// 解析器是"够用就好"的实现：完整支持字符串转义（含 \uXXXX 与代理对）、
// 数字、true/false/null、嵌套对象与数组、注释与尾逗号（配置文件是给人手改的）。
// ---------------------------------------------------------------------------

/// <summary>JSON 值的只读表示：对象 / 数组 / 字符串 / 数字 / 布尔 / null。</summary>
public sealed class JsonValue
{
    private readonly object? _value;

    private JsonValue(object? value) => _value = value;

    public static readonly JsonValue Null = new(null);
    public static readonly JsonValue True = new(true);
    public static readonly JsonValue False = new(false);

    public static JsonValue Of(string? value) => value is null ? Null : new JsonValue(value);
    public static JsonValue Of(double value) => new(value);
    public static JsonValue Of(bool value) => value ? True : False;
    public static JsonValue Of(JsonObject value) => new(value);
    public static JsonValue Of(JsonArray value) => new(value);

    public bool IsNull => _value is null;
    public bool IsObject => _value is JsonObject;
    public bool IsArray => _value is JsonArray;

    /// <summary>对象成员；不是对象或成员不存在时返回 <see cref="Null"/>。</summary>
    public JsonValue this[string name] =>
        _value is JsonObject obj && obj.TryGet(name, out var found) ? found : Null;

    /// <summary>数组元素；越界返回 <see cref="Null"/>。</summary>
    public JsonValue this[int index] =>
        _value is JsonArray array && index >= 0 && index < array.Count ? array[index] : Null;

    public bool TryGet(string name, out JsonValue value)
    {
        if (_value is JsonObject obj && obj.TryGet(name, out var found))
        {
            value = found;
            return true;
        }
        value = Null;
        return false;
    }

    public int Count => _value is JsonArray array ? array.Count : 0;

    /// <summary>集合成员名（仅对象）。</summary>
    public IEnumerable<string> Names =>
        _value is JsonObject obj ? obj.Names : Enumerable.Empty<string>();

    /// <summary>数组元素（仅数组）。</summary>
    public IEnumerable<JsonValue> Items =>
        _value is JsonArray array ? array.Items : Enumerable.Empty<JsonValue>();

    public string? AsString() => _value as string;

    public bool AsBool(bool fallback = false) => _value is bool b ? b : fallback;

    public double AsDouble(double fallback = 0) => _value is double d ? d : fallback;

    public int AsInt(int fallback = 0) =>
        _value is double d ? (int)Math.Round(d, MidpointRounding.AwayFromZero) : fallback;

    public override string ToString() => _value switch
    {
        null => "null",
        string s => s,
        bool b => b ? "true" : "false",
        double d => d.ToString(CultureInfo.InvariantCulture),
        _ => _value.GetType().Name,
    };
}

/// <summary>有序的 JSON 对象（保留写入顺序，配置文件的字段顺序才有意义）。</summary>
public sealed class JsonObject
{
    private readonly List<KeyValuePair<string, JsonValue>> _members = new();
    private readonly Dictionary<string, int> _index = new(StringComparer.Ordinal);

    public int Count => _members.Count;

    public IEnumerable<string> Names
    {
        get
        {
            foreach (var member in _members) yield return member.Key;
        }
    }

    public JsonObject Set(string name, JsonValue value)
    {
        if (_index.TryGetValue(name, out var at))
        {
            _members[at] = new KeyValuePair<string, JsonValue>(name, value);
        }
        else
        {
            _index[name] = _members.Count;
            _members.Add(new KeyValuePair<string, JsonValue>(name, value));
        }
        return this;
    }

    public JsonObject Set(string name, string? value)
    {
        if (value is not null) Set(name, JsonValue.Of(value));
        return this;
    }

    public JsonObject Set(string name, bool value) => Set(name, JsonValue.Of(value));

    public JsonObject Set(string name, int value) => Set(name, JsonValue.Of((double)value));

    public JsonObject Set(string name, JsonArray value) => Set(name, JsonValue.Of(value));

    public JsonObject Set(string name, JsonObject value) => Set(name, JsonValue.Of(value));

    public bool TryGet(string name, out JsonValue value)
    {
        if (_index.TryGetValue(name, out var at))
        {
            value = _members[at].Value;
            return true;
        }
        value = JsonValue.Null;
        return false;
    }

    public JsonValue Get(string name) => TryGet(name, out var value) ? value : JsonValue.Null;
}

public sealed class JsonArray
{
    private readonly List<JsonValue> _items = new();

    public int Count => _items.Count;

    public IEnumerable<JsonValue> Items
    {
        get
        {
            foreach (var item in _items) yield return item;
        }
    }

    public JsonValue this[int index] =>
        index >= 0 && index < _items.Count ? _items[index] : JsonValue.Null;

    public JsonArray Add(JsonValue value)
    {
        _items.Add(value);
        return this;
    }

    public JsonArray Add(string value) => Add(JsonValue.Of(value));
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

public static class JsonWriter
{
    /// <summary>把值序列化成缩进格式的 JSON 文本（行尾用 \n，与项目其它产物一致）。</summary>
    public static string Write(JsonValue value)
    {
        var builder = new StringBuilder(1024);
        WriteValue(builder, value, 0);
        return builder.ToString();
    }

    public static void WriteValue(StringBuilder builder, JsonValue value, int depth)
    {
        if (value.IsObject) WriteObject(builder, value, depth);
        else if (value.IsArray) WriteArray(builder, value, depth);
        else WriteScalar(builder, value);
    }

    private static void WriteObject(StringBuilder builder, JsonValue value, int depth)
    {
        var names = value.Names.ToList();
        if (names.Count == 0)
        {
            builder.Append("{}");
            return;
        }

        builder.Append("{\n");
        for (var i = 0; i < names.Count; i++)
        {
            Indent(builder, depth + 1);
            WriteString(builder, names[i]);
            builder.Append(": ");
            WriteValue(builder, value[names[i]], depth + 1);
            if (i < names.Count - 1) builder.Append(',');
            builder.Append('\n');
        }
        Indent(builder, depth);
        builder.Append('}');
    }

    private static void WriteArray(StringBuilder builder, JsonValue value, int depth)
    {
        var items = value.Items.ToList();
        if (items.Count == 0)
        {
            builder.Append("[]");
            return;
        }

        builder.Append("[\n");
        for (var i = 0; i < items.Count; i++)
        {
            Indent(builder, depth + 1);
            WriteValue(builder, items[i], depth + 1);
            if (i < items.Count - 1) builder.Append(',');
            builder.Append('\n');
        }
        Indent(builder, depth);
        builder.Append(']');
    }

    private static void WriteScalar(StringBuilder builder, JsonValue value)
    {
        if (value.IsNull)
        {
            builder.Append("null");
            return;
        }

        if (value.AsString() is { } text)
        {
            WriteString(builder, text);
            return;
        }

        if (value.AsDouble(double.NaN) is var number && !double.IsNaN(number))
        {
            // 整数不带小数点：配置里的 version 应该是 1 而不是 1.0
            if (number == Math.Floor(number) && Math.Abs(number) < 1e15)
            {
                builder.Append(((long)number).ToString(CultureInfo.InvariantCulture));
            }
            else
            {
                builder.Append(number.ToString("R", CultureInfo.InvariantCulture));
            }
            return;
        }

        builder.Append(value.AsBool() ? "true" : "false");
    }

    private static void Indent(StringBuilder builder, int depth) => builder.Append(' ', depth * 2);

    /// <summary>写字符串字面量：只转义必需的字符，非 ASCII 原样输出。</summary>
    public static void WriteString(StringBuilder builder, string text)
    {
        builder.Append('"');
        foreach (var c in text)
        {
            switch (c)
            {
                case '"': builder.Append("\\\""); break;
                case '\\': builder.Append("\\\\"); break;
                case '\b': builder.Append("\\b"); break;
                case '\f': builder.Append("\\f"); break;
                case '\n': builder.Append("\\n"); break;
                case '\r': builder.Append("\\r"); break;
                case '\t': builder.Append("\\t"); break;
                default:
                    if (c < 0x20)
                    {
                        builder.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        builder.Append(c);
                    }
                    break;
            }
        }
        builder.Append('"');
    }
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

/// <summary>JSON 语法错误；消息里带上出错位置，方便定位手改坏的配置文件。</summary>
public sealed class JsonParseException : Exception
{
    public JsonParseException(string message) : base(message) { }
}

public static class JsonParser
{
    /// <summary>解析一段 JSON 文本；失败时抛 <see cref="JsonParseException"/>。</summary>
    public static JsonValue Parse(string text)
    {
        var parser = new Parser(text);
        parser.SkipTrivia();
        var value = parser.ReadValue();
        parser.SkipTrivia();
        if (!parser.AtEnd) parser.Fail("文档结束后还有多余内容");
        return value;
    }

    private sealed class Parser
    {
        private const int MaxDepth = 64;

        private readonly string _text;
        private int _pos;

        public Parser(string text)
        {
            _text = text;
        }

        public bool AtEnd => _pos >= _text.Length;

        public void Fail(string message) =>
            throw new JsonParseException($"位置 {_pos + 1}：{message}");

        /// <summary>跳过空白、// 行注释与 /* 块注释。</summary>
        public void SkipTrivia()
        {
            while (_pos < _text.Length)
            {
                var c = _text[_pos];

                if (char.IsWhiteSpace(c))
                {
                    _pos++;
                    continue;
                }

                if (c == '/' && _pos + 1 < _text.Length)
                {
                    if (_text[_pos + 1] == '/')
                    {
                        _pos += 2;
                        while (_pos < _text.Length && _text[_pos] != '\n') _pos++;
                        continue;
                    }

                    if (_text[_pos + 1] == '*')
                    {
                        _pos += 2;
                        while (_pos + 1 < _text.Length && !(_text[_pos] == '*' && _text[_pos + 1] == '/')) _pos++;
                        _pos = Math.Min(_pos + 2, _text.Length);
                        continue;
                    }
                }

                break;
            }
        }

        /// <summary>
        /// 读完一个成员 / 元素后，吃掉分隔用的逗号。
        ///
        /// 逗号必须在这里处理，不能混进 <see cref="SkipTrivia"/>：
        /// 普通逗号后面跟着的是下一个成员名（不是 } 或 ]），
        /// 在 SkipTrivia 里根本区分不出"分隔逗号"和"尾逗号"，
        /// 结果会把分隔逗号留下，解析器卡在逗号上报"成员名必须是字符串"。
        ///
        /// 放在这里就能明确知道"一个成员已经读完了"：
        /// 有逗号就吃掉（后面跟 } 或 ] 就是手改配置常见的尾逗号，同样放行），
        /// 没逗号就直接进入闭合判断。
        /// </summary>
        private void SkipSeparator()
        {
            SkipTrivia();
            if (_pos < _text.Length && _text[_pos] == ',') _pos++;
            SkipTrivia();
        }

        public JsonValue ReadValue()
        {
            if (AtEnd) Fail("期待一个值，但已经到文档结尾");

            return _text[_pos] switch
            {
                '{' => ReadObject(0),
                '[' => ReadArray(0),
                '"' => JsonValue.Of(ReadString()),
                't' => ReadLiteral("true", JsonValue.True),
                'f' => ReadLiteral("false", JsonValue.False),
                'n' => ReadLiteral("null", JsonValue.Null),
                _ => ReadNumber(),
            };
        }

        private JsonValue ReadLiteral(string literal, JsonValue value)
        {
            if (_pos + literal.Length > _text.Length ||
                string.CompareOrdinal(_text, _pos, literal, 0, literal.Length) != 0)
            {
                Fail($"无法识别的字面量（期待 {literal}）");
            }
            _pos += literal.Length;
            return value;
        }

        private JsonValue ReadObject(int depth)
        {
            if (depth > MaxDepth) Fail("嵌套层级过深");
            _pos++;   // '{'

            var result = new JsonObject();
            while (true)
            {
                SkipTrivia();
                if (AtEnd) Fail("对象没有闭合的 }");
                if (_text[_pos] == '}') { _pos++; return JsonValue.Of(result); }

                if (_text[_pos] != '"') Fail("对象的成员名必须是字符串");
                var name = ReadString();

                SkipTrivia();
                if (AtEnd || _text[_pos] != ':') Fail("成员名后面应当是 :");
                _pos++;

                SkipTrivia();
                result.Set(name, ReadValue());

                SkipSeparator();
            }
        }

        private JsonValue ReadArray(int depth)
        {
            if (depth > MaxDepth) Fail("嵌套层级过深");
            _pos++;   // '['

            var result = new JsonArray();
            while (true)
            {
                SkipTrivia();
                if (AtEnd) Fail("数组没有闭合的 ]");
                if (_text[_pos] == ']') { _pos++; return JsonValue.Of(result); }

                result.Add(ReadValue());

                SkipSeparator();
            }
        }

        private string ReadString()
        {
            _pos++;   // '"'
            var builder = new StringBuilder();

            while (true)
            {
                if (AtEnd) Fail("字符串没有闭合的引号");
                var c = _text[_pos++];

                if (c == '"') return builder.ToString();

                if (c != '\\')
                {
                    builder.Append(c);
                    continue;
                }

                if (AtEnd) Fail("转义序列不完整");
                var escape = _text[_pos++];
                switch (escape)
                {
                    case '"': builder.Append('"'); break;
                    case '\\': builder.Append('\\'); break;
                    case '/': builder.Append('/'); break;
                    case 'b': builder.Append('\b'); break;
                    case 'f': builder.Append('\f'); break;
                    case 'n': builder.Append('\n'); break;
                    case 'r': builder.Append('\r'); break;
                    case 't': builder.Append('\t'); break;
                    case 'u':
                        var code = ReadHex4();
                        // 代理对：把高位和低位合成一个字符
                        if (code >= 0xD800 && code <= 0xDBFF &&
                            _pos + 1 < _text.Length && _text[_pos] == '\\' && _text[_pos + 1] == 'u')
                        {
                            var save = _pos;
                            _pos += 2;
                            var low = ReadHex4();
                            if (low >= 0xDC00 && low <= 0xDFFF)
                            {
                                builder.Append((char)code).Append((char)low);
                                break;
                            }
                            _pos = save;
                        }
                        builder.Append((char)code);
                        break;
                    default:
                        Fail($"无法识别的转义字符 \\{escape}");
                        break;
                }
            }
        }

        private int ReadHex4()
        {
            if (_pos + 4 > _text.Length) Fail("\\u 转义不完整");
            var value = 0;
            for (var i = 0; i < 4; i++)
            {
                var c = _text[_pos + i];
                var digit = c switch
                {
                    >= '0' and <= '9' => c - '0',
                    >= 'a' and <= 'f' => c - 'a' + 10,
                    >= 'A' and <= 'F' => c - 'A' + 10,
                    _ => -1,
                };
                if (digit < 0) Fail("\\u 转义里不是十六进制数字");
                value = (value << 4) | digit;
            }
            _pos += 4;
            return value;
        }

        private JsonValue ReadNumber()
        {
            var start = _pos;
            if (_pos < _text.Length && (_text[_pos] == '-' || _text[_pos] == '+')) _pos++;

            while (_pos < _text.Length)
            {
                var c = _text[_pos];
                if (char.IsDigit(c) || c is '.' or 'e' or 'E' or '+' or '-') _pos++;
                else break;
            }

            var span = _text.Substring(start, _pos - start);
            if (!double.TryParse(span, NumberStyles.Float, CultureInfo.InvariantCulture, out var number))
            {
                Fail($"不是合法的数字：{span}");
            }
            return JsonValue.Of(number);
        }
    }
}
