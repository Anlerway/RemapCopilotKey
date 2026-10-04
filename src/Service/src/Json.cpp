// ---------------------------------------------------------------------------
// RemapCopilotKey - 极简 JSON 解析 / 序列化模块实现
// ---------------------------------------------------------------------------

#include "Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rck {
namespace json {

namespace {

constexpr size_t kMaxDepth = 64;

// 把一个 Unicode 码位按 UTF-8 追加到字符串
void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ---------------------------------------------------------------------------
// 递归下降解析器
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(const std::string& text, std::string& error) : m_s(text), m_err(error) {}

    bool Run(Value& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        if (m_i < m_s.size()) return Fail("顶层值之后出现多余字符");
        return true;
    }

private:
    const std::string& m_s;
    std::string&       m_err;
    size_t             m_i = 0;

    bool Eof() const { return m_i >= m_s.size(); }
    char Peek() const { return m_i < m_s.size() ? m_s[m_i] : '\0'; }

    bool Fail(const char* msg) {
        size_t line = 1, col = 1;
        const size_t upto = m_i < m_s.size() ? m_i : m_s.size();
        for (size_t k = 0; k < upto; ++k) {
            if (m_s[k] == '\n') { ++line; col = 1; } else { ++col; }
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf), "%s (line %llu, column %llu)",
                      msg, static_cast<unsigned long long>(line),
                      static_cast<unsigned long long>(col));
        m_err = buf;
        return false;
    }

    void SkipWs() {
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_i;
            else break;
        }
    }

    bool Expect(char c, const char* what) {
        if (Eof() || m_s[m_i] != c) return Fail(what);
        ++m_i;
        return true;
    }

    bool Literal(const char* lit, const char* what) {
        const size_t n = std::strlen(lit);
        if (m_i + n > m_s.size() || m_s.compare(m_i, n, lit) != 0) return Fail(what);
        m_i += n;
        return true;
    }

    bool ParseValue(Value& out, size_t depth) {
        if (depth > kMaxDepth) return Fail("嵌套层级过深");
        if (Eof()) return Fail("期待一个 JSON 值，但已到末尾");
        switch (Peek()) {
        case '{': return ParseObject(out, depth);
        case '[': return ParseArray(out, depth);
        case '"': {
            std::string s;
            if (!ParseString(s)) return false;
            out.setString(std::move(s));
            return true;
        }
        case 't': if (!Literal("true",  "期待 'true'"))  return false; out.setBool(true);  return true;
        case 'f': if (!Literal("false", "期待 'false'")) return false; out.setBool(false); return true;
        case 'n': if (!Literal("null",  "期待 'null'"))  return false; out.setNull();     return true;
        default:
            if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) return ParseNumber(out);
            return Fail("无法识别的 JSON 值起始字符");
        }
    }

    bool ParseObject(Value& out, size_t depth) {
        if (!Expect('{', "期待 '{'")) return false;
        out.setObject();
        SkipWs();
        if (Peek() == '}') { ++m_i; return true; }
        for (;;) {
            SkipWs();
            if (Peek() != '"') return Fail("对象的键必须是字符串");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (!Expect(':', "键之后期待 ':'")) return false;
            SkipWs();
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.set(key, std::move(v));
            SkipWs();
            if (Peek() == ',') { ++m_i; continue; }
            if (Peek() == '}') { ++m_i; return true; }
            return Fail("对象中期待 ',' 或 '}'");
        }
    }

    bool ParseArray(Value& out, size_t depth) {
        if (!Expect('[', "期待 '['")) return false;
        out.setArray();
        SkipWs();
        if (Peek() == ']') { ++m_i; return true; }
        for (;;) {
            SkipWs();
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.push(std::move(v));
            SkipWs();
            if (Peek() == ',') { ++m_i; continue; }
            if (Peek() == ']') { ++m_i; return true; }
            return Fail("数组中期待 ',' 或 ']'");
        }
    }

    bool ParseHex4(unsigned int& cp) {
        if (m_i + 4 > m_s.size()) return Fail("\\u 转义不完整");
        unsigned int v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_s[m_i + k];
            if (c >= '0' && c <= '9')      v = v * 16 + static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + static_cast<unsigned int>(c - 'A' + 10);
            else return Fail("\\u 之后不是合法的十六进制数字");
        }
        m_i += 4;
        cp = v;
        return true;
    }

    bool ParseString(std::string& out) {
        if (!Expect('"', "期待 '\"'")) return false;
        out.clear();
        for (;;) {
            if (Eof()) return Fail("字符串没有闭合");
            const unsigned char c = static_cast<unsigned char>(m_s[m_i++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (Eof()) return Fail("转义序列不完整");
                const char e = m_s[m_i++];
                switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // 高位代理：如果后面紧跟低位代理则合成，否则替换为 U+FFFD
                        if (m_i + 1 < m_s.size() && m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
                            m_i += 2;
                            unsigned int lo = 0;
                            if (!ParseHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            } else {
                                AppendUtf8(out, 0xFFFD);
                                cp = lo;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("非法的转义序列");
                }
                continue;
            }
            if (c < 0x20) return Fail("字符串中出现未转义的控制字符");
            out += static_cast<char>(c);
        }
    }

    bool ParseNumber(Value& out) {
        const size_t start = m_i;
        if (Peek() == '-') ++m_i;
        if (Eof()) return Fail("数字不完整");
        if (Peek() == '0') {
            ++m_i;
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++m_i;
        } else {
            return Fail("数字格式非法");
        }
        if (!Eof() && Peek() == '.') {
            ++m_i;
            if (Eof() || !std::isdigit(static_cast<unsigned char>(Peek()))) return Fail("小数点后缺少数字");
            while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++m_i;
        }
        if (!Eof() && (Peek() == 'e' || Peek() == 'E')) {
            ++m_i;
            if (!Eof() && (Peek() == '+' || Peek() == '-')) ++m_i;
            if (Eof() || !std::isdigit(static_cast<unsigned char>(Peek()))) return Fail("指数部分缺少数字");
            while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++m_i;
        }
        const std::string num = m_s.substr(start, m_i - start);
        out.setNumber(std::strtod(num.c_str(), nullptr));
        return true;
    }
};

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

void EscapeInto(std::string& out, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                out += buf;
            } else {
                // 非 ASCII 字节原样输出，保持 UTF-8 可读（中文不会被转成 \uXXXX）
                out += static_cast<char>(c);
            }
        }
    }
}

void FormatNumber(std::string& out, double n) {
    if (!std::isfinite(n)) { out += "null"; return; }
    if (n == std::floor(n) && std::fabs(n) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.15g", n);
    out += buf;
}

void SerializeInto(std::string& out, const Value& v, int indent, int depth) {
    const bool pretty = indent > 0;
    switch (v.type()) {
    case Value::Type::Null:   out += "null"; break;
    case Value::Type::Bool:   out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: FormatNumber(out, v.asNumber()); break;
    case Value::Type::String: out += '"'; EscapeInto(out, v.asString()); out += '"'; break;

    case Value::Type::Array: {
        const auto& a = v.items();
        if (a.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out.append(static_cast<size_t>(depth + 1) * indent, ' '); }
            SerializeInto(out, a[i], indent, depth + 1);
        }
        if (pretty) { out += '\n'; out.append(static_cast<size_t>(depth) * indent, ' '); }
        out += ']';
        break;
    }

    case Value::Type::Object: {
        const auto& o = v.members();
        if (o.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < o.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out.append(static_cast<size_t>(depth + 1) * indent, ' '); }
            out += '"';
            EscapeInto(out, o[i].first);
            out += '"';
            out += ':';
            if (pretty) out += ' ';
            SerializeInto(out, o[i].second, indent, depth + 1);
        }
        if (pretty) { out += '\n'; out.append(static_cast<size_t>(depth) * indent, ' '); }
        out += '}';
        break;
    }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Value 成员
// ---------------------------------------------------------------------------

const Value::Array& Value::items() const {
    static const Array kEmpty;
    return m_type == Type::Array ? m_array : kEmpty;
}

const Value::Object& Value::members() const {
    static const Object kEmpty;
    return m_type == Type::Object ? m_object : kEmpty;
}

const Value* Value::find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    for (const auto& kv : m_object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

const Value& Value::at(const std::string& key) const {
    static const Value kNull;
    const Value* p = find(key);
    return p ? *p : kNull;
}

void Value::set(const std::string& key, Value v) {
    if (m_type != Type::Object) { m_type = Type::Object; m_object.clear(); }
    for (auto& kv : m_object) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    m_object.emplace_back(key, std::move(v));
}

void Value::push(Value v) {
    if (m_type != Type::Array) { m_type = Type::Array; m_array.clear(); }
    m_array.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

bool parse(const std::string& text, Value& out, std::string& error) {
    error.clear();
    Parser p(text, error);
    if (!p.Run(out)) {
        if (error.empty()) error = "JSON 解析失败";
        return false;
    }
    return true;
}

std::string serialize(const Value& v, int indent) {
    std::string out;
    SerializeInto(out, v, indent, 0);
    out += '\n';
    return out;
}

} // namespace json
} // namespace rck
