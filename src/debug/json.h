#pragma once
// A small JSON value for the debug adapter protocol and launch.json files: parse
// (optionally with // and /* */ comments and trailing commas, as VS Code allows),
// build and serialize. Objects keep their insertion order.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oh {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(size_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), str_(s) {}

    static Json array(std::vector<Json> items = {});
    static Json object(std::initializer_list<Member> members = {});

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Lenient accessors: a missing member or another type yields the fallback.
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::Number ? num_ : fallback; }
    int64_t integer(int64_t fallback = 0) const {
        return type_ == Type::Number ? static_cast<int64_t>(num_) : fallback;
    }
    const std::string& str() const;  // "" unless a string
    std::string str_or(const std::string& fallback) const { return is_string() ? str_ : fallback; }

    // Object member (a shared null when missing) and array element access.
    const Json& operator[](std::string_view key) const;
    const Json& operator[](size_t i) const;
    bool has(std::string_view key) const;
    size_t size() const;  // array items or object members
    const std::vector<Json>& items() const;      // empty unless an array
    const std::vector<Member>& members() const;  // empty unless an object

    // Building. set() replaces an existing member; push() appends to an array.
    Json& set(std::string key, Json value);
    Json& push(Json value);

    std::string dump() const;
    // Parses one JSON text. `relaxed` accepts comments and trailing commas (JSONC).
    static bool parse(std::string_view text, Json& out, std::string* error = nullptr, bool relaxed = false);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<Member> obj_;
    void dump_to(std::string& out) const;
};

// Escapes a string for inclusion in JSON (without the surrounding quotes).
std::string json_escape(std::string_view s);

} // namespace oh
