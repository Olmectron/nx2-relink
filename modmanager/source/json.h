// A small JSON reader and writer: what mod.json, port.json and mods.json contain, and no more.
//
// Reads objects, arrays, strings (with every escape, \uXXXX and surrogate pairs included), numbers,
// true, false and null; a UTF-8 byte order mark at the start is skipped; keys nobody asks for are
// kept and ignored. Text that is not valid UTF-8 is refused. Written text is ASCII only (other
// characters as \uXXXX), indented by one space a level, with "," and ": " as separators: the same
// bytes Python's json.dumps(value, indent=1) gives, so a file written here and one written by the
// PC tool differ in nothing but what they say.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolean = false;
    double number = 0.0;
    bool is_integer = false;   // the number was written without a fraction or exponent, and fits
    int64_t integer = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;   // in the order read (or to be written)

    bool is_null() const { return type == Null; }
    bool is_string() const { return type == String; }
    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    bool is_number() const { return type == Number; }

    /// A member of an object: the last of that name (as Python keeps it), or nullptr.
    const Value* get(const std::string& key) const;

    static Value make_string(const std::string& s);
    static Value make_integer(int64_t n);
    static Value make_array();
    static Value make_object();
    /// Adds a member to an object, or an element to an array (the key is then ignored).
    Value& add(const std::string& key, Value v);
};

/// Parses a whole text. False, with what is wrong and where in `error`, when it is not JSON.
bool parse(const std::string& text, Value& out, std::string& error);

/// The value as text (see the top of this file for its form).
std::string dump(const Value& value);

} // namespace json
