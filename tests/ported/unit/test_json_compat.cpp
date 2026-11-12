// Minimal JSON (spec 02 section 15).
//
// It exists for one purpose - PTSL carries command parameters and response
// bodies as JSON strings - so what matters is that it round-trips the shapes
// PTSL uses exactly, and refuses what it cannot represent rather than
// guessing.
#include "vcc/core/Json.h"

#include <string>

#include "vcc_test.h"

using namespace vcc::json;

VCC_TEST(the_scalar_kinds_parse_and_read_back)
{
    ParseError error;

    VCC_CHECK(parse_value("null", error).is_null());
    VCC_CHECK(!error.failed);

    VCC_CHECK(parse_value("true", error).as_bool());
    VCC_CHECK(!parse_value("false", error).as_bool(true));

    VCC_CHECK_NEAR(parse_value("42", error).as_number(), 42.0, 1e-12);
    VCC_CHECK_NEAR(parse_value("-17.5", error).as_number(), -17.5, 1e-12);
    VCC_CHECK_NEAR(parse_value("1.5e3", error).as_number(), 1500.0, 1e-12);
    VCC_CHECK_NEAR(parse_value("2E-2", error).as_number(), 0.02, 1e-12);

    VCC_CHECK_EQ(parse_value("\"hello\"", error).as_string(), std::string("hello"));
    VCC_CHECK(!error.failed);
}

VCC_TEST(a_value_of_the_wrong_kind_yields_the_fallback_rather_than_a_guess)
{
    // A caller reading a field the DAW did not send must be able to tell
    // that from a field it sent as zero.
    const Value text = Value::make_string("not a number");

    VCC_CHECK_NEAR(text.as_number(-1.0), -1.0, 1e-12);
    VCC_CHECK_EQ(text.as_int(-7), std::int64_t{-7});
    VCC_CHECK(text.as_bool(true));
    VCC_CHECK_EQ(Value::make_number(3.0).as_string_or("fallback"), std::string("fallback"));
    VCC_CHECK(Value::make_number(3.0).as_string().empty());

    VCC_CHECK_EQ(std::string(to_string(Kind::Object)), std::string("object"));
    VCC_CHECK_EQ(std::string(to_string(Kind::Number)), std::string("number"));
}

VCC_TEST(objects_and_arrays_read_by_key_and_index)
{
    const Value value = parse_value(R"({
        "session_name": "Reel 1",
        "sample_rate": 48000,
        "tracks": [
            {"name": "Violin I", "index": 0},
            {"name": "Cello", "index": 1}
        ],
        "is_dirty": true
    })");

    VCC_CHECK(value.is_object());
    VCC_CHECK_EQ(value["session_name"].as_string(), std::string("Reel 1"));
    VCC_CHECK_EQ(value["sample_rate"].as_int(), std::int64_t{48000});
    VCC_CHECK(value["is_dirty"].as_bool());

    VCC_CHECK(value["tracks"].is_array());
    VCC_CHECK_EQ(value["tracks"].size(), std::size_t{2});
    VCC_CHECK_EQ(value["tracks"].at(0)["name"].as_string(), std::string("Violin I"));
    VCC_CHECK_EQ(value["tracks"].at(1)["index"].as_int(), std::int64_t{1});
}

VCC_TEST(a_missing_field_is_safe_to_read_through)
{
    // Response bodies differ between Pro Tools versions, so reading a field
    // that is not there has to be ordinary rather than fatal.
    const Value value = parse_value(R"({"present": 1})");

    VCC_CHECK(!value.has("absent"));
    VCC_CHECK(value["absent"].is_null());
    VCC_CHECK_EQ(value["absent"]["deeper"]["deeper_still"].as_int(-1), std::int64_t{-1});
    VCC_CHECK_EQ(value["absent"].at(3).as_string(), std::string(""));
    VCC_CHECK_EQ(value["absent"].size(), std::size_t{0});
}

VCC_TEST(strings_survive_the_round_trip_including_what_needs_escaping)
{
    Value object = Value::make_object();
    object.set("quote", Value::make_string("a \"quoted\" name"));
    object.set("backslash", Value::make_string("C:\\Sessions\\Reel 1"));
    object.set("newline", Value::make_string("line one\nline two"));
    object.set("tab", Value::make_string("a\tb"));
    object.set("control", Value::make_string(std::string("bell\x07here")));
    // A track name a composer might really use.
    object.set("unicode", Value::make_string("Cordes \xE2\x99\xAD"));

    const std::string text = object.to_text();
    const Value back = parse_value(text);

    VCC_CHECK_EQ(back["quote"].as_string(), std::string("a \"quoted\" name"));
    VCC_CHECK_EQ(back["backslash"].as_string(), std::string("C:\\Sessions\\Reel 1"));
    VCC_CHECK_EQ(back["newline"].as_string(), std::string("line one\nline two"));
    VCC_CHECK_EQ(back["tab"].as_string(), std::string("a\tb"));
    VCC_CHECK_EQ(back["control"].as_string(), std::string("bell\x07here"));
    VCC_CHECK_EQ(back["unicode"].as_string(), std::string("Cordes \xE2\x99\xAD"));

    // UTF-8 passes through unescaped: escaping it would only make a request
    // unreadable to whoever is debugging it.
    VCC_CHECK(text.find("\xE2\x99\xAD") != std::string::npos);
}

VCC_TEST(escaped_unicode_including_surrogate_pairs_decodes)
{
    // A JSON escape is UTF-16, so a character outside the basic plane
    // arrives as two escapes that have to be recombined.
    const Value value = parse_value(R"({"flat": "\u266d", "clef": "\ud834\udd1e"})");

    VCC_CHECK_EQ(value["flat"].as_string(), std::string("\xE2\x99\xAD"));
    // U+1D11E, the G clef: four UTF-8 bytes.
    VCC_CHECK_EQ(value["clef"].as_string(), std::string("\xF0\x9D\x84\x9E"));
    VCC_CHECK_EQ(value["clef"].as_string().size(), std::size_t{4});
}

VCC_TEST(numbers_are_written_so_they_read_back_the_same)
{
    for (const double value : {0.0, 1.0, -1.0, 48000.0, 0.1, 1.0 / 3.0, 1e-7, 123456789.25}) {
        Value object = Value::make_object();
        object.set("value", Value::make_number(value));
        const Value back = parse_value(object.to_text());
        VCC_CHECK_MSG(back["value"].as_number() == value, object.to_text());
    }

    // A whole number is written without a decimal point: PTSL's numeric
    // fields are counts and identifiers, and "3.0" for a track index reads
    // like a mistake.
    Value index = Value::make_object();
    index.set("index", Value::make_number(3.0));
    VCC_CHECK_EQ(index.to_text(), std::string("{\"index\":3}"));
}

VCC_TEST(malformed_documents_are_refused_with_a_reason)
{
    struct Case {
        const char* text;
        const char* what;
    };
    const Case cases[] = {
        {"", "empty"},
        {"{", "unclosed object"},
        {"[", "unclosed array"},
        {"{\"a\"}", "no colon"},
        {"{\"a\":}", "no value"},
        {"{\"a\":1,}", "trailing comma"},
        {"[1,2,]", "trailing comma in array"},
        {"\"unterminated", "unterminated string"},
        {"nul", "truncated null"},
        {"+1", "leading plus"},
        {".5", "no integer part"},
        {"1.", "no fraction digits"},
        {"1e", "no exponent digits"},
        {"01", "trailing characters"},
        {"{\"a\":1} extra", "trailing content"},
        {"\"\\q\"", "unknown escape"},
        {"\"a\nb\"", "raw newline in a string"},
        {"'single'", "single quotes"},
        {"NaN", "not JSON"},
        {"Infinity", "not JSON"},
    };

    for (const Case& one : cases) {
        ParseError error;
        const Value value = parse_value(one.text, error);
        VCC_CHECK_MSG(error.failed, one.what);
        VCC_CHECK_MSG(!error.message.empty(), one.what);
        VCC_CHECK_MSG(value.is_null(), one.what);
    }
}

VCC_TEST(nesting_deeper_than_the_limit_is_refused_rather_than_recursed_into)
{
    // A corrupt or hostile document must not be able to exhaust the stack.
    std::string deep;
    for (int index = 0; index < kMaxDepth + 10; ++index)
        deep += '[';
    for (int index = 0; index < kMaxDepth + 10; ++index)
        deep += ']';

    ParseError error;
    const Value value = parse_value(deep, error);
    VCC_CHECK(error.failed);
    VCC_CHECK(value.is_null());
    VCC_CHECK(error.message.find("deep") != std::string::npos);

    // And a document just inside the limit still parses.
    std::string shallow;
    for (int index = 0; index < 30; ++index)
        shallow += '[';
    shallow += '1';
    for (int index = 0; index < 30; ++index)
        shallow += ']';

    ParseError ok;
    VCC_CHECK(parse_value(shallow, ok).is_array());
    VCC_CHECK(!ok.failed);
}

VCC_TEST(an_empty_object_or_array_is_not_the_same_as_a_missing_one)
{
    const Value value = parse_value(R"({"empty_object": {}, "empty_array": []})");

    VCC_CHECK(value.has("empty_object"));
    VCC_CHECK(value["empty_object"].is_object());
    VCC_CHECK_EQ(value["empty_object"].size(), std::size_t{0});

    VCC_CHECK(value["empty_array"].is_array());
    VCC_CHECK(!value["empty_array"].is_null());

    VCC_CHECK_EQ(Value::make_object().to_text(), std::string("{}"));
    VCC_CHECK_EQ(Value::make_array().to_text(), std::string("[]"));
}

VCC_TEST(keys_keep_their_order_so_a_request_is_stable)
{
    // Two runs of the application must produce the same request bytes for
    // the same command, or a log is impossible to compare.
    Value object = Value::make_object();
    object.set("zebra", Value::make_number(1));
    object.set("apple", Value::make_number(2));
    object.set("middle", Value::make_number(3));

    VCC_CHECK_EQ(object.to_text(),
                 std::string("{\"zebra\":1,\"apple\":2,\"middle\":3}"));

    // Setting an existing key replaces it without moving it.
    object.set("zebra", Value::make_number(9));
    VCC_CHECK_EQ(object.to_text(),
                 std::string("{\"zebra\":9,\"apple\":2,\"middle\":3}"));
    VCC_CHECK_EQ(object.keys().size(), std::size_t{3});
}

VCC_TEST(the_indented_form_is_for_reading_and_parses_back)
{
    Value inner = Value::make_object();
    inner.set("name", Value::make_string("Violin I"));

    Value list = Value::make_array();
    list.push_back(std::move(inner));

    Value root = Value::make_object();
    root.set("tracks", std::move(list));
    root.set("count", Value::make_number(1));

    const std::string pretty = root.to_text(2);
    VCC_CHECK(pretty.find('\n') != std::string::npos);

    const Value back = parse_value(pretty);
    VCC_CHECK_EQ(back["tracks"].at(0)["name"].as_string(), std::string("Violin I"));
    VCC_CHECK_EQ(back["count"].as_int(), std::int64_t{1});

    // The wire form carries no formatting: no newlines, and no space after
    // a colon or a comma. Spaces inside a value stay, since "Violin I" is
    // the track's name.
    const std::string compact = root.to_text();
    VCC_CHECK(compact.find('\n') == std::string::npos);
    VCC_CHECK(compact.find("\": ") == std::string::npos);
    VCC_CHECK(compact.find(", ") == std::string::npos);
    VCC_CHECK(compact.find("Violin I") != std::string::npos);
}

VCC_TEST(whitespace_between_tokens_is_ignored)
{
    const Value value = parse_value("  {\n\t\"a\" :\r\n [ 1 , 2 ]  }  ");
    VCC_CHECK(value.is_object());
    VCC_CHECK_EQ(value["a"].size(), std::size_t{2});
    VCC_CHECK_EQ(value["a"].at(1).as_int(), std::int64_t{2});
}

VCC_TEST(a_parse_error_says_where_it_gave_up)
{
    ParseError error;
    parse_value(R"({"good": 1, "bad": })", error);
    VCC_CHECK(error.failed);
    // Far enough in to point at the problem rather than at the start.
    VCC_CHECK(error.offset > 15);
}

VCC_TEST(removing_a_member_takes_it_out_of_the_keys_too)
{
    vcc::json::Value object = vcc::json::Value::make_object();
    object.set("a", vcc::json::Value::make_number(1));
    object.set("b", vcc::json::Value::make_number(2));
    object.set("c", vcc::json::Value::make_number(3));

    VCC_CHECK(object.remove("b"));
    VCC_CHECK(!object.has("b"));
    VCC_CHECK(object["b"].is_null());
    VCC_CHECK_EQ(object.keys().size(), std::size_t{2});
    VCC_CHECK(object.keys()[0] == "a" && object.keys()[1] == "c");
    VCC_CHECK(object.to_text() == "{\"a\":1,\"c\":3}");

    // Nothing to remove, and not an object: false, and nothing changed.
    VCC_CHECK(!object.remove("b"));
    vcc::json::Value number = vcc::json::Value::make_number(4);
    VCC_CHECK(!number.remove("a"));
    VCC_CHECK(number.is_number());
}

VCC_TEST_MAIN("shared/Json")
