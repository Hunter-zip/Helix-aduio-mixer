#include <filesystem>

#include "TestFramework.h"
#include "helix/app/Json.h"

using helix::app::JsonValue;

TEST("json/parsuje wartości proste") {
    CHECK(JsonValue::parse("null").isNull());
    CHECK(JsonValue::parse("true").asBool() == true);
    CHECK(JsonValue::parse("false").asBool() == false);
    CHECK_NEAR(JsonValue::parse("42").asNumber(), 42.0, 1e-12);
    CHECK_NEAR(JsonValue::parse("-3.5e2").asNumber(), -350.0, 1e-9);
    CHECK_STR_EQ(JsonValue::parse("\"tekst\"").asString(), "tekst");
}

TEST("json/obiekt zachowuje kolejność kluczy") {
    const JsonValue value = JsonValue::parse(R"({"b":1,"a":2,"c":3})");
    CHECK_EQ(value.members().size(), std::size_t{3});
    CHECK_STR_EQ(value.members()[0].first, "b");
    CHECK_STR_EQ(value.members()[1].first, "a");
    CHECK_STR_EQ(value.members()[2].first, "c");
}

TEST("json/zagnieżdżone struktury") {
    const JsonValue value = JsonValue::parse(R"({"a":[1,2,{"b":"x"}],"c":{"d":null}})");
    CHECK_EQ(value["a"].size(), std::size_t{3});
    CHECK_STR_EQ(value["a"][2]["b"].asString(), "x");
    CHECK(value["c"]["d"].isNull());
    CHECK(value["nie-ma"].isNull());
    CHECK(value["a"][99].isNull());
}

TEST("json/sekwencje ucieczki i unicode") {
    const JsonValue value = JsonValue::parse(R"("a\"b\\c\ndą😀")");
    const std::string text = value.asString();
    CHECK(text.find("a\"b\\c\nd") == 0);
    // ą = U+0105 → 0xC4 0x85
    CHECK(text.find("\xC4\x85") != std::string::npos);
    // 😀 = U+1F600 → 0xF0 0x9F 0x98 0x80
    CHECK(text.find("\xF0\x9F\x98\x80") != std::string::npos);
}

TEST("json/serializacja i ponowny odczyt") {
    JsonValue root = JsonValue::makeObject();
    root.set("name", JsonValue("Helix \"Mixer\"\n"));
    root.set("volume", JsonValue(0.8f));
    root.set("count", JsonValue(7));
    root.set("flag", JsonValue(true));

    JsonValue list = JsonValue::makeArray();
    list.push(JsonValue(1)).push(JsonValue(2.5)).push(JsonValue("trzy"));
    root.set("list", std::move(list));

    const std::string text = root.dump(2);
    std::string error;
    const JsonValue parsed = JsonValue::parse(text, &error);

    CHECK_MSG(error.empty(), "błąd parsowania: " + error);
    CHECK_STR_EQ(parsed["name"].asString(), "Helix \"Mixer\"\n");
    CHECK_NEAR(parsed["volume"].asNumber(), 0.8, 1e-9);
    CHECK_EQ(parsed["count"].asInt(), 7);
    CHECK(parsed["flag"].asBool());
    CHECK_EQ(parsed["list"].size(), std::size_t{3});
}

TEST("json/float nie zostawia śmieci po konwersji") {
    JsonValue value = JsonValue::makeObject();
    value.set("v", JsonValue(0.8f));
    const std::string text = value.dump();
    CHECK_MSG(text.find("0.8000000") == std::string::npos, "brzydki zapis float: " + text);
    CHECK(text.find("0.8") != std::string::npos);
    // Odczyt musi wrócić dokładnie do tej samej wartości float.
    CHECK(static_cast<float>(JsonValue::parse(text)["v"].asNumber()) == 0.8f);
}

TEST("json/odrzuca błędne dokumenty") {
    const char* broken[] = {
        "{",  "[1,2", "{\"a\":}", "{a:1}", "\"niedomknięty", "01x", "tru", "{} extra",
        "[1,]", "{\"a\":1,}",
    };
    for (const char* text : broken) {
        std::string error;
        JsonValue::parse(text, &error);
        CHECK_MSG(!error.empty(), std::string("brak błędu dla: ") + text);
    }
}

TEST("json/chroni przed zbyt głębokim zagnieżdżeniem") {
    std::string text(200, '[');
    std::string error;
    JsonValue::parse(text, &error);
    CHECK(!error.empty());
}

TEST("json/zapis do pliku jest atomowy") {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "helix-json-test";
    fs::remove_all(directory);

    const std::string path = (directory / "nested" / "config.json").string();

    JsonValue value = JsonValue::makeObject();
    value.set("profile", JsonValue("Gaming"));

    std::string error;
    CHECK_MSG(helix::app::writeJsonFile(path, value, error), error);
    CHECK(fs::exists(path));
    CHECK_MSG(!fs::exists(path + ".tmp"), "plik tymczasowy nie został usunięty");

    JsonValue loaded;
    CHECK_MSG(helix::app::readJsonFile(path, loaded, error), error);
    CHECK_STR_EQ(loaded["profile"].asString(), "Gaming");

    // Nadpisanie istniejącego pliku musi się udać.
    value.set("profile", JsonValue("Streaming"));
    CHECK_MSG(helix::app::writeJsonFile(path, value, error), error);
    CHECK_MSG(helix::app::readJsonFile(path, loaded, error), error);
    CHECK_STR_EQ(loaded["profile"].asString(), "Streaming");

    fs::remove_all(directory);
}
