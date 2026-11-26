#include "lsp/json.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace oh::json {

namespace {

const Value& null_value() {
    static const Value v;
    return v;
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
    std::string_view t;
    size_t i = 0;
    bool ok = true;
    int depth = 0;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
    }
    bool lit(std::string_view w) {
        if (t.substr(i, w.size()) != w) return false;
        i += w.size();
        return true;
    }
    int hex4() {
        if (i + 4 > t.size()) { ok = false; return 0; }
        int v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else { ok = false; return 0; }
        }
        return v;
    }
    std::string string() {
        std::string out;
        i++;  // opening quote
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = static_cast<uint32_t>(hex4());
                    if (cp >= 0xD800 && cp < 0xDC00 && t.substr(i, 2) == "\\u") {
                        i += 2;
                        uint32_t lo = static_cast<uint32_t>(hex4());
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out += e; break;  // \" \\ \/
            }
        }
        ok = false;
        return out;
    }
    Value value() {
        Value v;
        ws();
        if (i >= t.size() || ++depth > 256) { ok = false; return v; }
        char c = t[i];
        if (c == '{') {
            v.type = Value::Object;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') { i++; depth--; return v; }
            while (ok) {
                ws();
                if (i >= t.size() || t[i] != '"') { ok = false; break; }
                std::string key = string();
                ws();
                if (i >= t.size() || t[i] != ':') { ok = false; break; }
                i++;
                v.obj.emplace_back(std::move(key), value());
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; break; }
                ok = false;
            }
        } else if (c == '[') {
            v.type = Value::Array;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') { i++; depth--; return v; }
            while (ok) {
                v.arr.push_back(value());
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; break; }
                ok = false;
            }
        } else if (c == '"') {
            v.type = Value::String;
            v.s = string();
        } else if (lit("true")) {
            v.type = Value::Bool;
            v.b = true;
        } else if (lit("false")) {
            v.type = Value::Bool;
        } else if (lit("null")) {
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            i++;
            while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.' || t[i] == 'e' ||
                                    t[i] == 'E' || t[i] == '+' || t[i] == '-'))
                i++;
            v.type = Value::Number;
            v.n = std::strtod(std::string(t.substr(start, i - start)).c_str(), nullptr);
        } else {
            ok = false;
        }
        depth--;
        return v;
    }
};

void dump_to(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Number: {
            char buf[32];
            if (v.n == static_cast<double>(static_cast<long long>(v.n)))
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.n));
            else
                std::snprintf(buf, sizeof(buf), "%.17g", v.n);
            out += buf;
            break;
        }
        case Value::String: out += quote(v.s); break;
        case Value::Array:
            out += '[';
            for (size_t k = 0; k < v.arr.size(); k++) {
                if (k) out += ',';
                dump_to(v.arr[k], out);
            }
            out += ']';
            break;
        case Value::Object:
            out += '{';
            for (size_t k = 0; k < v.obj.size(); k++) {
                if (k) out += ',';
                out += quote(v.obj[k].first);
                out += ':';
                dump_to(v.obj[k].second, out);
            }
            out += '}';
            break;
    }
}

} // namespace

const Value& Value::operator[](std::string_view key) const {
    if (type != Object) return null_value();
    for (auto& [k, v] : obj)
        if (k == key) return v;
    return null_value();
}

const Value& Value::operator[](size_t idx) const {
    if (type != Array || idx >= arr.size()) return null_value();
    return arr[idx];
}

bool Value::has(std::string_view key) const {
    if (type != Object) return false;
    for (auto& kv : obj)
        if (kv.first == key) return true;
    return false;
}

const std::string& Value::str() const {
    static const std::string empty;
    return type == String ? s : empty;
}

Value parse(std::string_view text, bool* ok) {
    Parser p{text};
    Value v = p.value();
    p.ws();
    if (p.i != text.size()) p.ok = false;
    if (ok) *ok = p.ok;
    return p.ok ? v : Value{};
}

std::string escape(std::string_view s) {
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string dump(const Value& v) {
    std::string out;
    dump_to(v, out);
    return out;
}

} // namespace oh::json
