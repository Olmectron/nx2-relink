#include "json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::get(const std::string& key) const {
    if (type != Object) {
        return nullptr;
    }
    for (size_t i = object.size(); i-- > 0;) {
        if (object[i].first == key) {
            return &object[i].second;
        }
    }
    return nullptr;
}

Value Value::make_string(const std::string& s) {
    Value v;
    v.type = String;
    v.string = s;
    return v;
}

Value Value::make_integer(int64_t n) {
    Value v;
    v.type = Number;
    v.is_integer = true;
    v.integer = n;
    v.number = static_cast<double>(n);
    return v;
}

Value Value::make_array() {
    Value v;
    v.type = Array;
    return v;
}

Value Value::make_object() {
    Value v;
    v.type = Object;
    return v;
}

Value& Value::add(const std::string& key, Value v) {
    if (type == Array) {
        array.push_back(std::move(v));
        return array.back();
    }
    object.emplace_back(key, std::move(v));
    return object.back().second;
}

namespace {

const int kMaxDepth = 64;

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;
    const char* start = nullptr;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s at byte %ld", what, static_cast<long>(p - start));
            error = buf;
        }
        return false;
    }

    void skip_space() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    static void append_utf8(std::string& s, uint32_t c) {
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (end - p < 4) {
            return fail("a \\u escape is cut short");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail("a \\u escape has a character that is not a hex digit");
            }
        }
        return true;
    }

    bool string(std::string& out) {
        ++p; // the opening quote
        out.clear();
        for (;;) {
            if (p >= end) {
                return fail("a string is not closed");
            }
            const unsigned char c = static_cast<unsigned char>(*p++);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return fail("a control character inside a string");
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (p >= end) {
                return fail("a string is not closed");
            }
            const char e = *p++;
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
                uint32_t u = 0;
                if (!hex4(u)) {
                    return false;
                }
                if (u >= 0xD800 && u < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char* back = p;
                    p += 2;
                    uint32_t low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    if (low >= 0xDC00 && low < 0xE000) {
                        u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        p = back; // (not its second half: read on its own next)
                    }
                }
                if (u >= 0xD800 && u < 0xE000) {
                    u = 0xFFFD; // (half a pair alone is no character)
                }
                append_utf8(out, u);
                break;
            }
            default:
                return fail("an escape that JSON does not have");
            }
        }
    }

    bool number(Value& out) {
        const char* from = p;
        bool integer = true;
        if (p < end && *p == '-') {
            ++p;
        }
        if (p >= end || *p < '0' || *p > '9') {
            return fail("something that is not a JSON value");
        }
        if (*p == '0') {
            ++p;
        } else {
            while (p < end && *p >= '0' && *p <= '9') {
                ++p;
            }
        }
        if (p < end && *p == '.') {
            integer = false;
            ++p;
            if (p >= end || *p < '0' || *p > '9') {
                return fail("a number has no digit after its point");
            }
            while (p < end && *p >= '0' && *p <= '9') {
                ++p;
            }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            integer = false;
            ++p;
            if (p < end && (*p == '+' || *p == '-')) {
                ++p;
            }
            if (p >= end || *p < '0' || *p > '9') {
                return fail("a number has no digit in its exponent");
            }
            while (p < end && *p >= '0' && *p <= '9') {
                ++p;
            }
        }
        const std::string text(from, static_cast<size_t>(p - from));
        out.type = Value::Number;
        out.number = std::strtod(text.c_str(), nullptr);
        out.is_integer = false;
        if (integer) {
            errno = 0;
            const long long n = std::strtoll(text.c_str(), nullptr, 10);
            if (errno == 0) {
                out.is_integer = true;
                out.integer = n;
            }
        }
        return true;
    }

    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, word, n) != 0) {
            return fail("something that is not a JSON value");
        }
        p += n;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nested too deeply");
        }
        skip_space();
        if (p >= end) {
            return fail("the text ends where a value was expected");
        }
        switch (*p) {
        case '{': {
            ++p;
            out.type = Value::Object;
            skip_space();
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                skip_space();
                if (p >= end || *p != '"') {
                    return fail("a member's name (a string) was expected");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skip_space();
                if (p >= end || *p != ':') {
                    return fail("':' was expected after a member's name");
                }
                ++p;
                out.object.emplace_back(std::move(key), Value());
                if (!value(out.object.back().second, depth + 1)) {
                    return false;
                }
                skip_space();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') {
                    ++p;
                    return true;
                }
                return fail("',' or '}' was expected");
            }
        }
        case '[': {
            ++p;
            out.type = Value::Array;
            skip_space();
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            for (;;) {
                out.array.emplace_back();
                if (!value(out.array.back(), depth + 1)) {
                    return false;
                }
                skip_space();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') {
                    ++p;
                    return true;
                }
                return fail("',' or ']' was expected");
            }
        }
        case '"':
            out.type = Value::String;
            return string(out.string);
        case 't':
            out.type = Value::Bool;
            out.boolean = true;
            return literal("true");
        case 'f':
            out.type = Value::Bool;
            out.boolean = false;
            return literal("false");
        case 'n':
            out.type = Value::Null;
            return literal("null");
        default:
            return number(out);
        }
    }
};

/// Whether bytes are well-formed UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF).
bool valid_utf8(const char* s, const char* end) {
    while (s < end) {
        const unsigned char c = static_cast<unsigned char>(*s);
        int more;
        uint32_t cp, least;
        if (c < 0x80) {
            ++s;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            more = 1;
            cp = c & 0x1F;
            least = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
            cp = c & 0x0F;
            least = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            more = 3;
            cp = c & 0x07;
            least = 0x10000;
        } else {
            return false;
        }
        if (end - s <= more) {
            return false;
        }
        for (int i = 1; i <= more; ++i) {
            const unsigned char t = static_cast<unsigned char>(s[i]);
            if ((t & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (t & 0x3F);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
            return false;
        }
        s += more + 1;
    }
    return true;
}

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* end = p + s.size();
    char buf[16];
    while (p < end) {
        const unsigned char c = *p;
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\b') {
            out += "\\b";
        } else if (c == '\f') {
            out += "\\f";
        } else if (c < 0x20) {
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            // one character of UTF-8, written as \uXXXX (two of them past U+FFFF)
            const int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
            uint32_t cp = more == 1 ? (c & 0x1F) : more == 2 ? (c & 0x0F) : (c & 0x07);
            bool good = more > 0 && end - p > more;
            for (int i = 1; good && i <= more; ++i) {
                good = (p[i] & 0xC0) == 0x80;
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            if (!good) {
                out += "\\ufffd"; // (bytes that are no character)
                ++p;
                continue;
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", static_cast<unsigned>(0xD800 + (cp >> 10)),
                              static_cast<unsigned>(0xDC00 + (cp & 0x3FF)));
            } else {
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(cp));
            }
            out += buf;
            p += more + 1;
            continue;
        }
        ++p;
    }
    out += '"';
}

void dump_value(std::string& out, const Value& v, int level) {
    char buf[64];
    switch (v.type) {
    case Value::Null:
        out += "null";
        break;
    case Value::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case Value::Number:
        if (v.is_integer) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.integer));
        } else if (std::isfinite(v.number)) {
            std::snprintf(buf, sizeof buf, "%.17g", v.number);
        } else {
            std::snprintf(buf, sizeof buf, "null");
        }
        out += buf;
        break;
    case Value::String:
        dump_string(out, v.string);
        break;
    case Value::Array:
        if (v.array.empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (size_t i = 0; i < v.array.size(); ++i) {
            out.append(static_cast<size_t>(level + 1), ' ');
            dump_value(out, v.array[i], level + 1);
            out += i + 1 < v.array.size() ? ",\n" : "\n";
        }
        out.append(static_cast<size_t>(level), ' ');
        out += ']';
        break;
    case Value::Object:
        if (v.object.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (size_t i = 0; i < v.object.size(); ++i) {
            out.append(static_cast<size_t>(level + 1), ' ');
            dump_string(out, v.object[i].first);
            out += ": ";
            dump_value(out, v.object[i].second, level + 1);
            out += i + 1 < v.object.size() ? ",\n" : "\n";
        }
        out.append(static_cast<size_t>(level), ' ');
        out += '}';
        break;
    }
}

} // namespace

bool parse(const std::string& text, Value& out, std::string& error) {
    out = Value();
    Parser ps;
    ps.start = text.data();
    ps.p = ps.start;
    ps.end = ps.start + text.size();
    if (text.size() >= 3 && std::memcmp(ps.p, "\xEF\xBB\xBF", 3) == 0) {
        ps.p += 3;
    }
    if (!valid_utf8(ps.p, ps.end)) {
        error = "not UTF-8 text";
        return false;
    }
    if (!ps.value(out, 0)) {
        error = ps.error;
        return false;
    }
    ps.skip_space();
    if (ps.p != ps.end) {
        ps.fail("something after the end of the value");
        error = ps.error;
        return false;
    }
    return true;
}

std::string dump(const Value& value) {
    std::string out;
    dump_value(out, value, 0);
    return out;
}

} // namespace json
