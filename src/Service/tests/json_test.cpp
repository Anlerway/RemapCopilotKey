// ---------------------------------------------------------------------------
// RemapCopilotKey - JSON 模块单元测试
//
// 自带的极小测试框架：不引入 gtest 等外部依赖。
// 运行方式见 tests/run-tests.cmd
// ---------------------------------------------------------------------------

#include "../src/Json.h"

#include <cstdio>
#include <string>

using rck::json::Value;

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

#define CHECK_EQ_STR(a, b)                                                       \
    do {                                                                         \
        const std::string _a = (a);                                               \
        const std::string _b = (b);                                               \
        if (_a == _b) { ++g_pass; }                                               \
        else {                                                                    \
            ++g_fail;                                                             \
            std::printf("  [FAIL] %s:%d  %s\n          实际: [%s]\n          期望: [%s]\n", \
                        __FILE__, __LINE__, #a, _a.c_str(), _b.c_str());          \
        }                                                                         \
    } while (0)

// ---------------------------------------------------------------------------

static void TestBasicScalars() {
    std::printf("TestBasicScalars\n");
    Value v;
    std::string err;

    CHECK(rck::json::parse("true", v, err) && v.isBool() && v.asBool());
    CHECK(rck::json::parse("false", v, err) && v.isBool() && !v.asBool());
    CHECK(rck::json::parse("null", v, err) && v.isNull());
    CHECK(rck::json::parse("  42  ", v, err) && v.isNumber() && v.asInt() == 42);

    // 整数必须原样输出，不能变成 42.000000
    CHECK_EQ_STR(rck::json::serialize(v, 0), "42\n");

    CHECK(rck::json::parse("-17", v, err) && v.asInt() == -17);
    CHECK(rck::json::parse("3.5", v, err) && v.asNumber() == 3.5);
    CHECK(rck::json::parse("1e3", v, err) && v.asNumber() == 1000.0);
    CHECK(rck::json::parse("-2.5E-2", v, err) && v.asNumber() == -0.025);

    // 类型不符时取默认值
    CHECK(rck::json::parse("\"x\"", v, err) && v.asInt(7) == 7);
}

static void TestStringsAndUnicode() {
    std::printf("TestStringsAndUnicode\n");
    Value v;
    std::string err;

    CHECK(rck::json::parse(R"("hello")", v, err));
    CHECK_EQ_STR(v.asString(), "hello");

    // 常规转义
    CHECK(rck::json::parse(R"("a\tb\nc\"d\\e\/f")", v, err));
    CHECK_EQ_STR(v.asString(), "a\tb\nc\"d\\e/f");

    // \u 转义 -> UTF-8（这里是 "中文"）
    CHECK(rck::json::parse(R"("\u4e2d\u6587")", v, err));
    CHECK_EQ_STR(v.asString(), "\xe4\xb8\xad\xe6\x96\x87");

    // 代理对 -> U+1F600 (😀)，UTF-8 为 F0 9F 98 80
    CHECK(rck::json::parse(R"("\ud83d\ude00")", v, err));
    CHECK_EQ_STR(v.asString(), "\xf0\x9f\x98\x80");

    // 单独的高位代理 -> U+FFFD
    CHECK(rck::json::parse(R"("\ud83d")", v, err));
    CHECK_EQ_STR(v.asString(), "\xef\xbf\xbd");

    // 直接写 UTF-8 中文也要能原样保留
    CHECK(rck::json::parse("\"\xe4\xb8\xad\xe6\x96\x87\"", v, err));
    CHECK_EQ_STR(v.asString(), "\xe4\xb8\xad\xe6\x96\x87");
    CHECK_EQ_STR(rck::json::serialize(v, 0), "\"\xe4\xb8\xad\xe6\x96\x87\"\n");

    // 控制字符必须被转义
    Value ctl;
    ctl.setString(std::string("a\x01z"));
    CHECK_EQ_STR(rck::json::serialize(ctl, 0), "\"a\\u0001z\"\n");
}

static void TestObjectsAndArrays() {
    std::printf("TestObjectsAndArrays\n");
    Value v;
    std::string err;

    const std::string src = R"({
        "version": 1,
        "language": "zh-CN",
        "rules": [
            { "id": "copilot", "enabled": true,
              "action": { "type": "sendKey", "vkey": "VK_RCONTROL" } }
        ]
    })";

    CHECK(rck::json::parse(src, v, err));
    if (!err.empty()) std::printf("    解析错误: %s\n", err.c_str());

    CHECK(v.isObject());
    CHECK(v.at("version").asInt() == 1);
    CHECK_EQ_STR(v.at("language").asString(), "zh-CN");
    CHECK(v.at("rules").isArray());
    CHECK(v.at("rules").items().size() == 1);

    const Value& r0 = v.at("rules").items().at(0);
    CHECK_EQ_STR(r0.at("id").asString(), "copilot");
    CHECK(r0.at("enabled").asBool());
    CHECK_EQ_STR(r0.at("action").at("type").asString(), "sendKey");
    CHECK_EQ_STR(r0.at("action").at("vkey").asString(), "VK_RCONTROL");

    // 不存在的键：find() 返回 nullptr，at() 返回 Null 且可继续链式访问
    CHECK(v.find("nope") == nullptr);
    CHECK(v.at("nope").isNull());
    CHECK(v.at("nope").at("deeper").isNull());
    CHECK(!v.has("nope"));

    // 空容器
    CHECK(rck::json::parse("{}", v, err) && v.isObject() && v.members().empty());
    CHECK(rck::json::parse("[]", v, err) && v.isArray() && v.items().empty());
    CHECK_EQ_STR(rck::json::serialize(v, 0), "[]\n");
}

static void TestRoundTrip() {
    std::printf("TestRoundTrip\n");
    Value a, b;
    std::string err;

    const std::string src = R"({
        "version": 1,
        "language": "zh-CN",
        "rules": [
            { "id": "copilot", "enabled": true, "note": "中文 & \"引号\"",
              "action": { "type": "sendChord", "keys": ["VK_CONTROL", "VK_SHIFT", "VK_ESCAPE"] } }
        ],
        "number": -12.5
    })";

    CHECK(rck::json::parse(src, a, err));

    const std::string pretty = rck::json::serialize(a, 2);
    CHECK(rck::json::parse(pretty, b, err));

    // 二次序列化必须完全一致（幂等）
    CHECK_EQ_STR(rck::json::serialize(b, 2), pretty);

    // 内容等价
    CHECK_EQ_STR(b.at("rules").items().at(0).at("note").asString(), "中文 & \"引号\"");
    CHECK_EQ_STR(b.at("rules").items().at(0).at("action").at("keys").items().at(2).asString(), "VK_ESCAPE");
    CHECK(b.at("number").asNumber() == -12.5);
}

static void TestErrors() {
    std::printf("TestErrors\n");
    Value v;
    std::string err;

    struct Case { const char* text; const char* why; };
    const Case cases[] = {
        { "",              "空输入" },
        { "{",             "对象未闭合" },
        { "[1,]",          "数组尾随逗号" },
        { "{\"a\":1,}",    "对象尾随逗号" },
        { "\"abc",         "字符串未闭合" },
        { "{a:1}",         "键不是字符串" },
        { "01",            "前导零" },
        { "1.",            "小数点后无数字" },
        { "1e",            "指数无数字" },
        { "\"\\x\"",       "非法转义" },
        { "\"\\u12\"",     "\\u 不完整" },
        { "tru",           "截断的字面量" },
        { "{} {}",         "多余内容" },
        { "nul",           "截断的 null" },
        { "[1 2]",         "数组缺逗号" },
    };

    for (const Case& c : cases) {
        err.clear();
        const bool ok = rck::json::parse(c.text, v, err);
        if (ok) {
            ++g_fail;
            std::printf("  [FAIL] 本应失败却解析成功: %s (%s)\n", c.text, c.why);
        } else {
            ++g_pass;
            if (err.empty()) {
                ++g_fail;
                std::printf("  [FAIL] 解析失败但没有给出错误信息: %s\n", c.text);
            }
        }
    }

    // 错误信息应包含行列号
    err.clear();
    rck::json::parse("{\n  \"a\": 1,\n  bad\n}", v, err);
    CHECK(err.find("line 3") != std::string::npos);

    // 深度限制：超过 64 层必须报错而不是栈溢出
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';
    err.clear();
    CHECK(!rck::json::parse(deep, v, err));
}

static void TestMutation() {
    std::printf("TestMutation\n");
    Value v = Value::MakeObject();
    v.set("version", Value(1));
    v.set("language", Value("zh-CN"));

    // 同键覆盖，不产生重复成员
    v.set("version", Value(2));
    CHECK(v.members().size() == 2);
    CHECK(v.at("version").asInt() == 2);

    Value arr = Value::MakeArray();
    arr.push(Value("a"));
    arr.push(Value(true));
    v.set("list", arr);
    CHECK(v.at("list").items().size() == 2);
    CHECK(v.at("list").items().at(1).asBool());
}

int main() {
    std::printf("=== RemapCopilotKey JSON 模块测试 ===\n\n");

    TestBasicScalars();
    TestStringsAndUnicode();
    TestObjectsAndArrays();
    TestRoundTrip();
    TestErrors();
    TestMutation();

    std::printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
