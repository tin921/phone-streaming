// A small JSON reader for the server's messages (objects, arrays, strings,
// numbers, true/false/null). A missing key or index reads as null.
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ps {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    static Json parse(const std::string& text) {
        size_t at = 0;
        Json v;
        if (!read(text, at, v, 0)) return Json{};
        return v;
    }

    Type type() const { return t; }
    bool isNull() const { return t == Type::Null; }
    bool isArray() const { return t == Type::Array; }
    bool isObject() const { return t == Type::Object; }
    double num(double fallback = 0) const { return t == Type::Number ? n : fallback; }
    bool truthy() const { return (t == Type::Bool && b) || (t == Type::Number && n != 0); }
    const std::string& str() const { static const std::string empty; return t == Type::String ? s : empty; }
    size_t size() const { return t == Type::Array ? items.size() : 0; }

    const Json& operator[](size_t i) const { return t == Type::Array && i < items.size() ? items[i] : none(); }
    const Json& operator[](const char* key) const {
        if (t == Type::Object) for (const auto& kv : fields) if (kv.first == key) return kv.second;
        return none();
    }

private:
    Type t = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    static const Json& none() { static const Json null; return null; }

    static void space(const std::string& x, size_t& at) {
        while (at < x.size() && (x[at] == ' ' || x[at] == '\t' || x[at] == '\n' || x[at] == '\r')) ++at;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }

    static bool hex4(const std::string& x, size_t at, unsigned& v) {
        if (at + 4 > x.size()) return false;
        v = 0;
        for (size_t i = at; i < at + 4; ++i) {
            char c = x[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static bool string(const std::string& x, size_t& at, std::string& out) {
        if (at >= x.size() || x[at] != '"') return false;
        ++at;
        while (at < x.size()) {
            char c = x[at++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (at >= x.size()) return false;
            char e = x[at++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(x, at, cp)) return false;
                    at += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && at + 6 <= x.size() && x[at] == '\\' && x[at + 1] == 'u') {
                        unsigned lo;
                        if (hex4(x, at + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            at += 6;
                        }
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        return false;
    }

    static bool read(const std::string& x, size_t& at, Json& v, int depth) {
        if (depth > 32) return false;
        space(x, at);
        if (at >= x.size()) return false;
        char c = x[at];
        if (c == '{') {
            v.t = Type::Object;
            ++at;
            space(x, at);
            if (at < x.size() && x[at] == '}') { ++at; return true; }
            for (;;) {
                space(x, at);
                std::string key;
                if (!string(x, at, key)) return false;
                space(x, at);
                if (at >= x.size() || x[at] != ':') return false;
                ++at;
                Json item;
                if (!read(x, at, item, depth + 1)) return false;
                v.fields.emplace_back(std::move(key), std::move(item));
                space(x, at);
                if (at < x.size() && x[at] == ',') { ++at; continue; }
                if (at < x.size() && x[at] == '}') { ++at; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = Type::Array;
            ++at;
            space(x, at);
            if (at < x.size() && x[at] == ']') { ++at; return true; }
            for (;;) {
                Json item;
                if (!read(x, at, item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                space(x, at);
                if (at < x.size() && x[at] == ',') { ++at; continue; }
                if (at < x.size() && x[at] == ']') { ++at; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = Type::String; return string(x, at, v.s); }
        if (x.compare(at, 4, "true") == 0) { v.t = Type::Bool; v.b = true; at += 4; return true; }
        if (x.compare(at, 5, "false") == 0) { v.t = Type::Bool; v.b = false; at += 5; return true; }
        if (x.compare(at, 4, "null") == 0) { v.t = Type::Null; at += 4; return true; }
        char* end = nullptr;
        double d = strtod(x.c_str() + at, &end);
        if (end == x.c_str() + at) return false;
        v.t = Type::Number;
        v.n = d;
        at = size_t(end - x.c_str());
        return true;
    }
};

} // namespace ps
