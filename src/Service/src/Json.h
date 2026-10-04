#pragma once
// ---------------------------------------------------------------------------
// RemapCopilotKey - 极简 JSON 解析 / 序列化模块
//
// 设计目标：
//   * 零外部依赖（不引入 nlohmann/json 之类的大文件）
//   * 只覆盖配置读写需要的 JSON 子集，但严格遵循 RFC 8259 语法
//   * 对象保留成员书写顺序（配置回写时不会乱序）
//   * UTF-8 原样透传，`\uXXXX` 转义（含代理对）正确解码为 UTF-8
//
// 本模块不做数值精度以外的"业务"校验，校验放在 Config 层。
// ---------------------------------------------------------------------------

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rck {
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array  = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(int n) : m_type(Type::Number), m_number(static_cast<double>(n)) {}
    Value(double n) : m_type(Type::Number), m_number(n) {}
    Value(const char* s) : m_type(Type::String), m_string(s ? s : "") {}
    Value(std::string s) : m_type(Type::String), m_string(std::move(s)) {}

    static Value MakeArray()  { Value v; v.m_type = Type::Array;  return v; }
    static Value MakeObject() { Value v; v.m_type = Type::Object; return v; }

    // --- 类型查询 ---------------------------------------------------------
    Type type()     const { return m_type; }
    bool isNull()   const { return m_type == Type::Null; }
    bool isBool()   const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray()  const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    // --- 取值（类型不符时返回默认值） -------------------------------------
    bool        asBool  (bool def = false) const { return m_type == Type::Bool   ? m_bool   : def; }
    double      asNumber(double def = 0.0) const { return m_type == Type::Number ? m_number : def; }
    int         asInt   (int def = 0)      const { return m_type == Type::Number ? static_cast<int>(m_number) : def; }
    std::string asString(const std::string& def = std::string()) const {
        return m_type == Type::String ? m_string : def;
    }

    const Array&  items()   const;
    const Object& members() const;

    // --- 对象访问 ---------------------------------------------------------
    // 找不到返回 nullptr；重复键返回第一个
    const Value* find(const std::string& key) const;
    // 找不到返回一个共享的 Null；可安全链式取值
    const Value& at(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }

    // --- 构造 / 修改 ------------------------------------------------------
    void set(const std::string& key, Value v);
    void push(Value v);

    void setNull()                { m_type = Type::Null; }
    void setBool(bool b)          { m_type = Type::Bool;   m_bool = b; }
    void setNumber(double n)      { m_type = Type::Number; m_number = n; }
    void setInt(int n)            { m_type = Type::Number; m_number = static_cast<double>(n); }
    void setString(std::string s) { m_type = Type::String; m_string = std::move(s); }
    void setArray()               { m_type = Type::Array;  m_array.clear(); }
    void setObject()              { m_type = Type::Object; m_object.clear(); }

private:
    Type        m_type   = Type::Null;
    bool        m_bool   = false;
    double      m_number = 0.0;
    std::string m_string;
    Array       m_array;
    Object      m_object;
};

// 解析失败时返回 false，并把「原因 (line N, column M)」写入 error。
// 最大嵌套深度限制为 64，防止恶意/损坏文件造成栈溢出。
bool parse(const std::string& text, Value& out, std::string& error);

// 缩进美化输出（indent <= 0 表示紧凑输出）。输出始终以换行结尾。
std::string serialize(const Value& v, int indent = 2);

} // namespace json
} // namespace rck
