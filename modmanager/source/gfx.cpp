#include "gfx.h"

#include <algorithm>

#include "font_data.h"

namespace gfx {

namespace {

const char kUnknown = '\x7f'; // (the box: the font's last picture)

// U+00C0 to U+00FF without their accents
const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";

} // namespace

std::string plain(const std::string& utf8) {
    std::string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const unsigned char* end = p + utf8.size();
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) {
            ++p;
            if (c == '\n' || c == '\t') {
                out += ' ';
            } else if (c >= 32 && c < 127) {
                out += static_cast<char>(c);
            } else if (c != '\r') {
                out += kUnknown;
            }
            continue;
        }
        const int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        uint32_t cp = more == 1 ? (c & 0x1F) : more == 2 ? (c & 0x0F) : (c & 0x07);
        ++p;
        int got = 0;
        while (got < more && p < end && (*p & 0xC0) == 0x80) {
            cp = (cp << 6) | (*p & 0x3F);
            ++p;
            ++got;
        }
        if (more == 0 || got != more) {
            out += kUnknown;
        } else if (cp >= 0xC0 && cp <= 0xFF) {
            out += kLatin1[cp - 0xC0];
        } else if (cp == 0x2018 || cp == 0x2019) {
            out += '\'';
        } else if (cp == 0x201C || cp == 0x201D) {
            out += '"';
        } else if (cp == 0x2013 || cp == 0x2014) {
            out += '-';
        } else if (cp == 0xA0) {
            out += ' ';
        } else if (cp == 0x200B || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F)) {
            // (nothing to see)
        } else {
            out += kUnknown;
        }
    }
    return out;
}

std::string clipped(const std::string& s, size_t most) {
    if (most == 0 || s.size() <= most) {
        return s;
    }
    if (most <= 3) {
        return s.substr(0, most);
    }
    return s.substr(0, most - 3) + "...";
}

std::vector<std::string> wrapped(const std::string& s, size_t width, size_t most_lines) {
    std::vector<std::string> lines;
    if (width == 0 || most_lines == 0) {
        return lines;
    }
    size_t at = 0;
    while (at < s.size()) {
        while (at < s.size() && s[at] == ' ') {
            ++at;
        }
        if (at >= s.size()) {
            break;
        }
        size_t take = std::min(width, s.size() - at);
        if (at + take < s.size() && s[at + take] != ' ') {
            const size_t space = s.rfind(' ', at + take);
            if (space != std::string::npos && space > at) {
                take = space - at;
            }
        }
        if (lines.size() + 1 == most_lines && at + take < s.size()) {
            lines.push_back(clipped(s.substr(at, width + 1), width));
            return lines;
        }
        std::string line = s.substr(at, take);
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        lines.push_back(line);
        at += take;
    }
    return lines;
}

int advance(int scale, size_t count) {
    return static_cast<int>(count) * (kFontW + 1) * scale;
}

int line_height(int scale) {
    return (kFontH + 1) * scale;
}

void Canvas::clear(uint32_t color) {
    std::fill(pixels.begin(), pixels.end(), color);
}

void Canvas::fill(int x, int y, int w, int h, uint32_t color) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0), x1 = std::min(x + w, width), y1 = std::min(y + h, height);
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t* row = pixels.data() + static_cast<size_t>(yy) * width;
        for (int xx = x0; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

void Canvas::frame(int x, int y, int w, int h, int t, uint32_t color) {
    fill(x, y, w, t, color);
    fill(x, y + h - t, w, t, color);
    fill(x, y, t, h, color);
    fill(x + w - t, y, t, h, color);
}

int Canvas::text(int x, int y, const std::string& utf8, int scale, uint32_t color, size_t most) {
    const std::string s = clipped(plain(utf8), most);
    for (const char ch : s) {
        const int c = static_cast<unsigned char>(ch);
        const int index = (c >= kFontFirst && c < kFontFirst + kFontCount) ? c - kFontFirst : kFontCount - 1;
        const unsigned char* rows = kFontRows[index];
        for (int r = 0; r < kFontH; ++r) {
            const unsigned bits = rows[r];
            if (!bits) {
                continue;
            }
            // (dots side by side are drawn as one box)
            for (int col = 0; col < kFontW;) {
                if (!(bits & (1u << (kFontW - 1 - col)))) {
                    ++col;
                    continue;
                }
                int run = 1;
                while (col + run < kFontW && (bits & (1u << (kFontW - 1 - col - run)))) {
                    ++run;
                }
                fill(x + col * scale, y + r * scale, run * scale, scale, color);
                col += run;
            }
        }
        x += (kFontW + 1) * scale;
    }
    return x;
}

void Canvas::text_right(int right, int y, const std::string& utf8, int scale, uint32_t color, size_t most) {
    const std::string s = clipped(plain(utf8), most);
    text(right - advance(scale, s.size()) + scale, y, s, scale, color);
}

void Canvas::text_centered(int middle, int y, const std::string& utf8, int scale, uint32_t color, size_t most) {
    const std::string s = clipped(plain(utf8), most);
    text(middle - (advance(scale, s.size()) - scale) / 2, y, s, scale, color);
}

} // namespace gfx
