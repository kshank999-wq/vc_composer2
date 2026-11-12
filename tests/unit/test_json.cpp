#include "vcc/core/Json.h"
#include "vcc_test.h"

using namespace vcc;

TEST_CASE("json round-trips nested values and keeps member order")
{
    const std::string text = R"({"b": 1, "a": [true, false, null, 2.5, "x"], "c": {"z": -3, "y": "q"}})";
    auto v = json::parse(text);
    REQUIRE(v);
    const auto& obj = v->as_object();
    REQUIRE(obj.size() == 3);
    CHECK_EQ(obj.begin()->first, std::string("b"));
    CHECK_EQ(v->get("c")->get_int("z"), -3);
    auto again = json::parse(json::write(*v));
    REQUIRE(again);
    CHECK(*again == *v);
    CHECK_EQ(json::write(*again), json::write(*v));
}

TEST_CASE("json keeps 64-bit integers exact and doubles as doubles")
{
    auto v = json::parse(R"({"tick": 9007199254740993, "gain": 1.0, "neg": -0.25})");
    REQUIRE(v);
    CHECK_EQ(v->get_int("tick"), 9007199254740993LL);
    CHECK(v->get("gain")->type() == json::Type::Double);
    std::string out = json::write(*v, {0});
    CHECK(out.find("9007199254740993") != std::string::npos);
    CHECK(out.find("1.0") != std::string::npos);
}

TEST_CASE("json decodes escapes and surrogate pairs to UTF-8")
{
    auto v = json::parse(R"(["\u00e9", "\ud83c\udfb5", "tab\tnew\nq\"s\\"])");
    REQUIRE(v);
    CHECK_EQ(v->as_array()[0].as_string(), std::string("\xC3\xA9"));
    CHECK_EQ(v->as_array()[1].as_string(), std::string("\xF0\x9F\x8E\xB5"));
    auto again = json::parse(json::write(*v));
    REQUIRE(again);
    CHECK(*again == *v);
}

TEST_CASE("json reports errors with line and column")
{
    json::ParseError err;
    CHECK(!json::parse("{\n  \"a\": 1,\n  \"b\": }", &err));
    CHECK_EQ(err.line, 3u);
    CHECK(!json::parse("[1, 2", &err));
    CHECK(!json::parse("{\"a\":1,\"a\":2}", &err));
    CHECK(err.message.find("duplicate") != std::string::npos);
    CHECK(!json::parse("01", &err));
    CHECK(!json::parse("\"\\ud800\"", &err));
    CHECK(!json::parse("[1] x", &err));
}

TEST_CASE("json refuses pathological nesting instead of overflowing the stack")
{
    std::string deep(10000, '[');
    json::ParseError err;
    CHECK(!json::parse(deep, &err));
    CHECK(err.message.find("deep") != std::string::npos);
}

TEST_CASE("json object set replaces in place")
{
    json::Object o{{"a", 1}, {"b", 2}};
    o.set("a", 5);
    CHECK_EQ(o.begin()->second.as_int(), 5);
    CHECK(o.erase("b"));
    CHECK(!o.contains("b"));
}
