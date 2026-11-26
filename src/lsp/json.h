#pragma once
// Minimal JSON DOM for LSP responses whose shape is nested (symbols, workspace
// edits, inlay hints, hover arrays). The older request parsers in client.cpp
// keep their key-search helpers; new code should parse with this instead.
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oh::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool is_null() const { return type == Null; }
    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    bool is_string() const { return type == String; }
    bool is_number() const { return type == Number; }
    // Missing keys / indexes yield a shared Null value, so lookups chain safely.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](size_t i) const;
    bool has(std::string_view key) const;
    size_t size() const { return type == Array ? arr.size() : type == Object ? obj.size() : 0; }
    int as_int(int def = 0) const { return type == Number ? static_cast<int>(n) : def; }
    bool as_bool(bool def = false) const { return type == Bool ? b : def; }
    const std::string& str() const;  // "" unless a string
};

// Parses one JSON document; Null on malformed input.
Value parse(std::string_view text, bool* ok = nullptr);
// Escapes for embedding inside a JSON string literal (no surrounding quotes).
std::string escape(std::string_view s);
// "..." with escaping.
inline std::string quote(std::string_view s) { return "\"" + escape(s) + "\""; }
// Serializes a value back to compact JSON (used to echo request ids).
std::string dump(const Value& v);

} // namespace oh::json
