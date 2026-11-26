#include "tasks/json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oh::json {

namespace {
const Value& null_value() {
    static const Value v;
    return v;
}
const std::string empty_string;
const std::vector<Value> empty_items;
const std::vector<std::pair<std::string, Value>> empty_members;

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
} // namespace

const std::string& Value::str() const { return type_ == Type::String ? s_ : empty_string; }
const std::vector<Value>& Value::items() const { return type_ == Type::Array ? arr_ : empty_items; }
const std::vector<std::pair<std::string, Value>>& Value::members() const {
    return type_ == Type::Object ? obj_ : empty_members;
}
const Value& Value::operator[](size_t i) const {
    return type_ == Type::Array && i < arr_.size() ? arr_[i] : null_value();
}
const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null_value();
    for (auto& [k, v] : obj_)
        if (k == key) return v;
    return null_value();
}
bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

Value Value::make_string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.s_ = std::move(s);
    return v;
}
Value Value::make_number(double n) {
    Value v;
    v.type_ = Type::Number;
    v.n_ = n;
    return v;
}
Value Value::make_bool(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.b_ = b;
    return v;
}
Value Value::make_array(std::vector<Value> items) {
    Value v;
    v.type_ = Type::Array;
    v.arr_ = std::move(items);
    return v;
}
Value Value::make_object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}
void Value::push(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ == Type::Array) arr_.push_back(std::move(v));
}
void Value::set(const std::string& key, Value v) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) return;
    for (auto& kv : obj_)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    obj_.emplace_back(key, std::move(v));
}

class Parser {
public:
    explicit Parser(const std::string& t) : s(t) {}
    const std::string& s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void skip() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
            } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                size_t end = s.find("*/", i + 2);
                i = end == std::string::npos ? s.size() : end + 2;
            } else {
                return;
            }
        }
    }
    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return false;
        out = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i + static_cast<size_t>(k)];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        i += 4;
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return fail("bad surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v) {
        if (++depth > 200) return fail("nesting too deep");
        skip();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            v.type_ = Value::Type::Object;
            i++;
            for (;;) {
                skip();
                if (i < s.size() && s[i] == '}') { i++; break; }
                std::string key;
                if (!string(key)) { ok = false; break; }
                skip();
                if (i >= s.size() || s[i] != ':') { ok = fail("expected ':'"); break; }
                i++;
                Value member;
                if (!value(member)) { ok = false; break; }
                v.obj_.emplace_back(std::move(key), std::move(member));
                skip();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                ok = fail("expected ',' or '}'");
                break;
            }
        } else if (c == '[') {
            v.type_ = Value::Type::Array;
            i++;
            for (;;) {
                skip();
                if (i < s.size() && s[i] == ']') { i++; break; }
                Value item;
                if (!value(item)) { ok = false; break; }
                v.arr_.push_back(std::move(item));
                skip();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                ok = fail("expected ',' or ']'");
                break;
            }
        } else if (c == '"') {
            v.type_ = Value::Type::String;
            ok = string(v.s_);
        } else if (s.compare(i, 4, "true") == 0) {
            v.type_ = Value::Type::Bool;
            v.b_ = true;
            i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            v.type_ = Value::Type::Bool;
            i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            v.type_ = Value::Type::Number;
            v.n_ = std::strtod(start, &end);
            if (end == start) ok = fail("bad number");
            i += static_cast<size_t>(end - start);
        } else {
            ok = fail("unexpected character");
        }
        depth--;
        return ok;
    }
};

bool parse(const std::string& text, Value& out, std::string* error) {
    Parser p(text);
    out = Value();
    bool ok = p.value(out);
    if (ok) {
        p.skip();
        if (p.i != text.size()) ok = p.fail("trailing characters");
    }
    if (!ok && error) *error = p.err;
    return ok;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string dump(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return v.boolean() ? "true" : "false";
        case Value::Type::Number: {
            double n = v.number();
            char buf[32];
            if (std::floor(n) == n && std::fabs(n) < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
            else std::snprintf(buf, sizeof buf, "%.17g", n);
            return buf;
        }
        case Value::Type::String: return quote(v.str());
        case Value::Type::Array: {
            std::string out = "[";
            for (size_t k = 0; k < v.items().size(); k++) out += (k ? "," : "") + dump(v.items()[k]);
            return out + "]";
        }
        case Value::Type::Object: {
            std::string out = "{";
            bool first = true;
            for (auto& [key, val] : v.members()) {
                out += (first ? "" : ",") + quote(key) + ":" + dump(val);
                first = false;
            }
            return out + "}";
        }
    }
    return "null";
}

} // namespace oh::json
