// Minimalny, kompletny JSON (spec §24) — bez zależności zewnętrznych.
//
// Obiekty zachowują kolejność kluczy, żeby zapisany profil był stabilny
// i czytelny w diffie.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix::app {

class JsonValue {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    using Array  = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool value) : type_(Type::Bool), bool_(value) {}
    JsonValue(double value) : type_(Type::Number), number_(value) {}
    JsonValue(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    JsonValue(unsigned value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    JsonValue(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    JsonValue(unsigned long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    /// Wartości pochodzące z float są przycinane do najkrótszego zapisu,
    /// który wraca na tę samą liczbę float — bez tego profil pełen jest
    /// artefaktów typu 0.8000000119.
    JsonValue(float value) : type_(Type::Number), number_(narrowFloat(value)) {}
    JsonValue(const char* value) : type_(Type::String), string_(value ? value : "") {}
    JsonValue(std::string value) : type_(Type::String), string_(std::move(value)) {}
    JsonValue(std::string_view value) : type_(Type::String), string_(value) {}

    static JsonValue makeObject() { JsonValue v; v.type_ = Type::Object; return v; }
    static JsonValue makeArray()  { JsonValue v; v.type_ = Type::Array;  return v; }

    static JsonValue array(std::initializer_list<JsonValue> items) {
        JsonValue v = makeArray();
        v.array_.assign(items.begin(), items.end());
        return v;
    }

    /// Parsuje tekst. Przy błędzie zwraca wartość Null i wypełnia `error`.
    static JsonValue parse(std::string_view text, std::string* error = nullptr);

    /// Serializuje. `indent > 0` włącza formatowanie z wcięciami.
    [[nodiscard]] std::string dump(int indent = 0) const;

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull()   const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool()   const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray()  const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool        asBool(bool fallback = false) const noexcept;
    [[nodiscard]] double      asNumber(double fallback = 0.0) const noexcept;
    [[nodiscard]] float       asFloat(float fallback = 0.0f) const noexcept;
    [[nodiscard]] int         asInt(int fallback = 0) const noexcept;
    [[nodiscard]] std::uint32_t asUint(std::uint32_t fallback = 0) const noexcept;
    [[nodiscard]] std::string asString(std::string fallback = {}) const;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;

    /// Dostęp tylko do odczytu; brakujący klucz zwraca wartość Null.
    const JsonValue& operator[](std::string_view key) const noexcept;
    const JsonValue& operator[](std::size_t index) const noexcept;

    /// Wstawia/nadpisuje pole obiektu (konwertuje wartość na obiekt, jeśli trzeba).
    JsonValue& set(std::string key, JsonValue value);

    /// Dokłada element tablicy (konwertuje wartość na tablicę, jeśli trzeba).
    JsonValue& push(JsonValue value);

    [[nodiscard]] const Object& members() const noexcept { return object_; }
    [[nodiscard]] const Array&  items()   const noexcept { return array_; }
    [[nodiscard]] Object& members() noexcept { return object_; }
    [[nodiscard]] Array&  items()   noexcept { return array_; }

private:
    static const JsonValue& nullValue() noexcept;
    static double narrowFloat(float value) noexcept;

    Type        type_ = Type::Null;
    bool        bool_ = false;
    double      number_ = 0.0;
    std::string string_;
    Array       array_;
    Object      object_;
};

/// Wczytuje/zapisuje plik JSON. Zapis jest atomowy (plik tymczasowy + rename),
/// dzięki czemu przerwany zapis nie niszczy konfiguracji (spec §23).
[[nodiscard]] bool readJsonFile(const std::string& path, JsonValue& out, std::string& error);
[[nodiscard]] bool writeJsonFile(const std::string& path, const JsonValue& value, std::string& error);

} // namespace helix::app
