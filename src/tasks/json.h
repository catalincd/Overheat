#pragma once
// A small JSON reader for task files (.vscode/tasks.json, CMakePresets.json), cargo's
// --message-format=json lines and LSP results that need more than the LSP client's
// key lookups (code actions, workspace edits). Accepts JSONC: // and /* */ comments
// and trailing commas. Numbers are doubles; objects keep their key order.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace oh::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool boolean(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double number(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    long long integer(long long def = 0) const { return type_ == Type::Number ? static_cast<long long>(n_) : def; }
    const std::string& str() const;  // "" unless a string
    std::string str_or(const std::string& def) const { return type_ == Type::String ? s_ : def; }

    // Arrays: elements (empty for other types).
    const std::vector<Value>& items() const;
    size_t size() const { return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0; }
    const Value& operator[](size_t i) const;

    // Objects: members in source order; lookups return a shared null when missing.
    const std::vector<std::pair<std::string, Value>>& members() const;
    const Value& operator[](const std::string& key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;

    // Builders (used by tests and the code-action glue).
    static Value make_string(std::string s);
    static Value make_number(double n);
    static Value make_bool(bool b);
    static Value make_array(std::vector<Value> v = {});
    static Value make_object();
    void push(Value v);
    void set(const std::string& key, Value v);

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
    friend class Parser;
};

// Parses `text`; returns false (and a message in *error) on malformed input.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);
// Serializes compactly (strings escaped, non-ASCII passed through as UTF-8).
std::string dump(const Value& v);
// A JSON string literal for `s`, quotes included.
std::string quote(const std::string& s);

} // namespace oh::json
