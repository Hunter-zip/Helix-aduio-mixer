#include "helix/app/Json.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace helix::app {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    Parser(std::string_view text, std::string& error) : text_(text), error_(error) {}

    bool run(JsonValue& out) {
        skipWhitespace();
        if (!parseValue(out, 0)) return false;
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("nieoczekiwane znaki na końcu dokumentu");
            return false;
        }
        return true;
    }

private:
    void fail(const std::string& message) {
        if (error_.empty())
            error_ = message + " (pozycja " + std::to_string(pos_) + ")";
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    [[nodiscard]] bool eof() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return text_[pos_]; }

    bool expect(char c) {
        if (eof() || text_[pos_] != c) {
            fail(std::string("oczekiwano '") + c + "'");
            return false;
        }
        ++pos_;
        return true;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) { fail("przekroczono dopuszczalne zagnieżdżenie"); return false; }
        skipWhitespace();
        if (eof()) { fail("nieoczekiwany koniec danych"); return false; }

        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string value;
                if (!parseString(value)) return false;
                out = JsonValue(std::move(value));
                return true;
            }
            case 't':
                if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; out = JsonValue(true); return true; }
                fail("nieznany literał");
                return false;
            case 'f':
                if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; out = JsonValue(false); return true; }
                fail("nieznany literał");
                return false;
            case 'n':
                if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; out = JsonValue(); return true; }
                fail("nieznany literał");
                return false;
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        if (!expect('{')) return false;
        out = JsonValue::makeObject();
        skipWhitespace();
        if (!eof() && peek() == '}') { ++pos_; return true; }

        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!expect(':')) return false;

            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            out.set(std::move(key), std::move(value));

            skipWhitespace();
            if (eof()) { fail("niedomknięty obiekt"); return false; }
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            fail("oczekiwano ',' lub '}'");
            return false;
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        if (!expect('[')) return false;
        out = JsonValue::makeArray();
        skipWhitespace();
        if (!eof() && peek() == ']') { ++pos_; return true; }

        while (true) {
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            out.push(std::move(value));

            skipWhitespace();
            if (eof()) { fail("niedomknięta tablica"); return false; }
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return true; }
            fail("oczekiwano ',' lub ']'");
            return false;
        }
    }

    static void appendUtf8(std::string& out, std::uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) { fail("skrócona sekwencja \\u"); return false; }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            std::uint32_t digit;
            if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
            else { fail("nieprawidłowa cyfra szesnastkowa"); return false; }
            out = (out << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        if (!expect('"')) return false;
        out.clear();

        while (true) {
            if (eof()) { fail("niedomknięty napis"); return false; }
            const char c = text_[pos_++];

            if (c == '"') return true;

            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) { fail("znak sterujący w napisie"); return false; }
                out.push_back(c);
                continue;
            }

            if (eof()) { fail("niedomknięta sekwencja ucieczki"); return false; }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!parseHex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // Para zastępcza — druga połowa jest obowiązkowa.
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t low = 0;
                            if (!parseHex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                            } else {
                                code = 0xFFFDu;  // uszkodzona para → znak zastępczy
                            }
                        } else {
                            code = 0xFFFDu;
                        }
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        code = 0xFFFDu;
                    }
                    appendUtf8(out, code);
                    break;
                }
                default:
                    fail("nieznana sekwencja ucieczki");
                    return false;
            }
        }
    }

    bool parseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (!eof() && (peek() == '-' || peek() == '+')) ++pos_;

        bool anyDigit = false;
        while (!eof() && peek() >= '0' && peek() <= '9') { ++pos_; anyDigit = true; }
        if (!eof() && peek() == '.') {
            ++pos_;
            while (!eof() && peek() >= '0' && peek() <= '9') { ++pos_; anyDigit = true; }
        }
        if (anyDigit && !eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            bool expDigit = false;
            while (!eof() && peek() >= '0' && peek() <= '9') { ++pos_; expDigit = true; }
            if (!expDigit) { fail("brak cyfr w wykładniku"); return false; }
        }

        if (!anyDigit) { fail("oczekiwano liczby"); return false; }

        const std::string token(text_.substr(start, pos_ - start));
        try {
            out = JsonValue(std::stod(token));
        } catch (...) {
            fail("liczba poza zakresem");
            return false;
        }
        return true;
    }

    std::string_view text_;
    std::string&     error_;
    std::size_t      pos_ = 0;
};

void escapeTo(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer.data();
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void numberTo(std::string& out, double value) {
    if (!std::isfinite(value)) { out += "null"; return; }

    // Liczby całkowite zapisujemy bez części dziesiętnej (czytelniejszy profil).
    if (value == std::floor(value) && std::fabs(value) < 1.0e15) {
        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%lld", static_cast<long long>(value));
        out += buffer.data();
        return;
    }

    // Najkrótszy zapis, który wczytuje się z powrotem na tę samą wartość —
    // profil zostaje czytelny, a odczyt jest bezstratny.
    std::array<char, 40> buffer{};
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buffer.data(), buffer.size(), "%.*g", precision, value);
        if (std::strtod(buffer.data(), nullptr) == value) break;
    }
    out += buffer.data();
}

void dumpTo(std::string& out, const JsonValue& value, int indent, int depth) {
    const std::string pad    = indent > 0 ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string padEnd = indent > 0 ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
    const char* newline = indent > 0 ? "\n" : "";
    const char* space   = indent > 0 ? " " : "";

    switch (value.type()) {
        case JsonValue::Type::Null:   out += "null"; break;
        case JsonValue::Type::Bool:   out += value.asBool() ? "true" : "false"; break;
        case JsonValue::Type::Number: numberTo(out, value.asNumber()); break;
        case JsonValue::Type::String: escapeTo(out, value.asString()); break;

        case JsonValue::Type::Array: {
            if (value.items().empty()) { out += "[]"; break; }
            out += '[';
            out += newline;
            bool first = true;
            for (const auto& item : value.items()) {
                if (!first) { out += ','; out += newline; }
                first = false;
                out += pad;
                dumpTo(out, item, indent, depth + 1);
            }
            out += newline;
            out += padEnd;
            out += ']';
            break;
        }

        case JsonValue::Type::Object: {
            if (value.members().empty()) { out += "{}"; break; }
            out += '{';
            out += newline;
            bool first = true;
            for (const auto& [key, member] : value.members()) {
                if (!first) { out += ','; out += newline; }
                first = false;
                out += pad;
                escapeTo(out, key);
                out += ':';
                out += space;
                dumpTo(out, member, indent, depth + 1);
            }
            out += newline;
            out += padEnd;
            out += '}';
            break;
        }
    }
}

} // namespace

double JsonValue::narrowFloat(float value) noexcept {
    if (!std::isfinite(value)) return static_cast<double>(value);

    std::array<char, 32> buffer{};
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buffer.data(), buffer.size(), "%.*g", precision, static_cast<double>(value));
        if (std::strtof(buffer.data(), nullptr) == value) break;
    }
    return std::strtod(buffer.data(), nullptr);
}

const JsonValue& JsonValue::nullValue() noexcept {
    static const JsonValue instance;
    return instance;
}

JsonValue JsonValue::parse(std::string_view text, std::string* error) {
    std::string localError;
    JsonValue result;
    Parser parser(text, localError);
    if (!parser.run(result)) {
        if (error) *error = localError;
        return JsonValue();
    }
    if (error) error->clear();
    return result;
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, *this, indent, 0);
    return out;
}

bool JsonValue::asBool(bool fallback) const noexcept {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

double JsonValue::asNumber(double fallback) const noexcept {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return fallback;
}

float JsonValue::asFloat(float fallback) const noexcept {
    return static_cast<float>(asNumber(static_cast<double>(fallback)));
}

int JsonValue::asInt(int fallback) const noexcept {
    return static_cast<int>(asNumber(static_cast<double>(fallback)));
}

std::uint32_t JsonValue::asUint(std::uint32_t fallback) const noexcept {
    const double value = asNumber(static_cast<double>(fallback));
    if (value < 0.0) return 0;
    return static_cast<std::uint32_t>(value);
}

std::string JsonValue::asString(std::string fallback) const {
    if (type_ == Type::String) return string_;
    return fallback;
}

std::size_t JsonValue::size() const noexcept {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

bool JsonValue::contains(std::string_view key) const noexcept {
    if (type_ != Type::Object) return false;
    for (const auto& [name, _] : object_)
        if (name == key) return true;
    return false;
}

const JsonValue& JsonValue::operator[](std::string_view key) const noexcept {
    if (type_ == Type::Object) {
        for (const auto& [name, value] : object_)
            if (name == key) return value;
    }
    return nullValue();
}

const JsonValue& JsonValue::operator[](std::size_t index) const noexcept {
    if (type_ == Type::Array && index < array_.size()) return array_[index];
    return nullValue();
}

JsonValue& JsonValue::set(std::string key, JsonValue value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        array_.clear();
    }
    for (auto& [name, existing] : object_) {
        if (name == key) {
            existing = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return *this;
}

JsonValue& JsonValue::push(JsonValue value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        object_.clear();
    }
    array_.push_back(std::move(value));
    return *this;
}

bool readJsonFile(const std::string& path, JsonValue& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "nie można otworzyć pliku: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    std::string parseError;
    out = JsonValue::parse(text, &parseError);
    if (!parseError.empty()) {
        error = path + ": " + parseError;
        return false;
    }
    return true;
}

bool writeJsonFile(const std::string& path, const JsonValue& value, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code code;

    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), code);
        if (code) {
            error = "nie można utworzyć katalogu: " + code.message();
            return false;
        }
    }

    // Zapis atomowy: najpierw plik tymczasowy, potem podmiana.
    const fs::path temporary = target.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "nie można zapisać pliku: " + temporary.string();
            return false;
        }
        file << value.dump(2) << '\n';
        if (!file) {
            error = "błąd zapisu do " + temporary.string();
            return false;
        }
    }

    fs::rename(temporary, target, code);
    if (code) {
        // Na Windows rename nie nadpisuje — spróbuj przez usunięcie celu.
        fs::remove(target, code);
        fs::rename(temporary, target, code);
        if (code) {
            error = "nie można podmienić pliku: " + code.message();
            return false;
        }
    }
    return true;
}

} // namespace helix::app
