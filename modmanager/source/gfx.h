// A plain picture in memory and the few things drawn on it: filled boxes and text in the
// program's own font (font_data.h, made by tools/make_font.py). The same code draws for the
// console's screen and for the pictures a PC writes to judge the screens by.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

/// A colour as the console's RGBA8 framebuffer has it: red in the lowest byte.
constexpr uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return (r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16) | 0xff000000u;
}

/// Text as the font can draw it: one byte a character. Line ends and tabs become spaces, the
/// accented Latin letters lose their accent, and what the font doesn't have becomes one box.
std::string plain(const std::string& utf8);

/// `plain` text cut to at most `most` characters, the last three of them dots when it was longer.
std::string clipped(const std::string& plain_text, size_t most);

/// `plain` text in lines of at most `width` characters, broken at spaces where it can be; at most
/// `most_lines` lines, the last ending in dots when there was more.
std::vector<std::string> wrapped(const std::string& plain_text, size_t width, size_t most_lines);

struct Canvas {
    int width, height;
    std::vector<uint32_t> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h) {}

    void clear(uint32_t color);
    void fill(int x, int y, int w, int h, uint32_t color);
    /// The outline of a box, `t` pixels thick, inside the box given.
    void frame(int x, int y, int w, int h, int t, uint32_t color);

    /// Draws text (UTF-8; made `plain` here) with its top left corner at x, y, each dot `scale`
    /// pixels a side, in at most `most` characters (0: no limit). Returns the x after it.
    int text(int x, int y, const std::string& utf8, int scale, uint32_t color, size_t most = 0);
    /// The same, ending at `right`.
    void text_right(int right, int y, const std::string& utf8, int scale, uint32_t color, size_t most = 0);
    /// The same, with its middle at `middle`.
    void text_centered(int middle, int y, const std::string& utf8, int scale, uint32_t color, size_t most = 0);
};

/// How wide `count` characters are at a scale, in pixels (a character and the gap after it).
int advance(int scale, size_t count = 1);
/// How high a line of text is at a scale, tails included.
int line_height(int scale);

} // namespace gfx
