#include "debug/json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oh {

namespace {
const Json& null_json() {
    static const Json n;
    return n;
}

void append_utf8(std::string& out, uint32_t cp) {
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

struct Parser {
    std::string_view s;
    bool relaxed;
    size_t i = 0;
    std::string error;
    int depth = 0;

    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }

    void skip_ws() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (!relaxed || i + 1 >= s.size() || s[i] != '/') return;
            if (s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
            } else if (s[i + 1] == '*') {
                size_t end = s.find("*/", i + 2);
                i = end == std::string_view::npos ? s.size() : end + 2;
            } else {
                return;
            }
        }
    }

    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        i++;  // opening quote
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
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else i = save;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(Json& out) {
        if (++depth > 512) return fail("nesting too deep");
        skip_ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            out = Json::object();
            i++;
            skip_ws();
            if (i < s.size() && s[i] == '}') {
                i++;
            } else {
                for (;;) {
                    skip_ws();
                    if (relaxed && i < s.size() && s[i] == '}') { i++; break; }  // trailing comma
                    if (i >= s.size() || s[i] != '"') { ok = fail("expected member name"); break; }
                    std::string key;
                    if (!string(key)) { ok = false; break; }
                    skip_ws();
                    if (i >= s.size() || s[i] != ':') { ok = fail("expected ':'"); break; }
                    i++;
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.set(std::move(key), std::move(v));
                    skip_ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == '}') { i++; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            out = Json::array();
            i++;
            skip_ws();
            if (i < s.size() && s[i] == ']') {
                i++;
            } else {
                for (;;) {
                    skip_ws();
                    if (relaxed && i < s.size() && s[i] == ']') { i++; break; }
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.push(std::move(v));
                    skip_ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == ']') { i++; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            std::string str;
            ok = string(str);
            out = Json(std::move(str));
        } else if (s.substr(i, 4) == "true") {
            out = Json(true);
            i += 4;
        } else if (s.substr(i, 5) == "false") {
            out = Json(false);
            i += 5;
        } else if (s.substr(i, 4) == "null") {
            out = Json();
            i += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            if (s[i] == '-') i++;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '+' || s[i] == '-'))
                i++;
            std::string num(s.substr(start, i - start));
            char* end = nullptr;
            double d = std::strtod(num.c_str(), &end);
            if (!end || *end) ok = fail("bad number");
            out = Json(d);
        } else {
            ok = fail("unexpected character");
        }
        depth--;
        return ok;
    }
};
} // namespace

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
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
    return out;
}

Json Json::array(std::vector<Json> items) {
    Json j;
    j.type_ = Type::Array;
    j.arr_ = std::move(items);
    return j;
}

Json Json::object(std::initializer_list<Member> members) {
    Json j;
    j.type_ = Type::Object;
    for (auto& m : members) j.set(m.first, m.second);
    return j;
}

const std::string& Json::str() const {
    static const std::string empty;
    return type_ == Type::String ? str_ : empty;
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ != Type::Object) return null_json();
    for (auto& m : obj_)
        if (m.first == key) return m.second;
    return null_json();
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return null_json();
    return arr_[i];
}

bool Json::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (auto& m : obj_)
        if (m.first == key) return true;
    return false;
}

size_t Json::size() const {
    return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0;
}

const std::vector<Json>& Json::items() const {
    static const std::vector<Json> empty;
    return type_ == Type::Array ? arr_ : empty;
}

const std::vector<Json::Member>& Json::members() const {
    static const std::vector<Member> empty;
    return type_ == Type::Object ? obj_ : empty;
}

Json& Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        *this = Json();
        type_ = Type::Object;
    }
    for (auto& m : obj_)
        if (m.first == key) {
            m.second = std::move(value);
            return *this;
        }
    obj_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        *this = Json();
        type_ = Type::Array;
    }
    arr_.push_back(std::move(value));
    return *this;
}

void Json::dump_to(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) < 9.007199254740992e15) {
                out += std::to_string(static_cast<int64_t>(num_));
            } else if (std::isfinite(num_)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            } else {
                out += "null";
            }
            break;
        }
        case Type::String:
            out += '"';
            out += json_escape(str_);
            out += '"';
            break;
        case Type::Array:
            out += '[';
            for (size_t k = 0; k < arr_.size(); k++) {
                if (k) out += ',';
                arr_[k].dump_to(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t k = 0; k < obj_.size(); k++) {
                if (k) out += ',';
                out += '"';
                out += json_escape(obj_[k].first);
                out += "\":";
                obj_[k].second.dump_to(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

bool Json::parse(std::string_view text, Json& out, std::string* error, bool relaxed) {
    Parser p{text, relaxed, 0, {}, 0};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) p.i = 3;  // UTF-8 BOM
    Json v;
    bool ok = p.value(v);
    if (ok) {
        p.skip_ws();
        if (p.i != text.size()) ok = p.fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = p.error;
        return false;
    }
    out = std::move(v);
    return true;
}

} // namespace oh
