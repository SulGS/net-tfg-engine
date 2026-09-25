#pragma once

#include <string>

// UTF-8 walking for UI text: strings stay std::string (UTF-8) and are decoded to codepoints only
// where glyphs are looked up. Byte offsets (text field cursor, selection) always sit on the start
// of a codepoint; Prev/NextBoundary keep them there.
namespace Utf8
{
    // Decodes the codepoint starting at text[i] and moves i past it. A byte that doesn't start a
    // valid sequence decodes as itself (Latin-1), so text that isn't UTF-8 still shows up.
    inline char32_t Next(const std::string& text, size_t& i) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if (b0 < 0x80) {
            ++i;
            return b0;
        }
        else if (b0 >= 0xC2 && b0 <= 0xDF) { length = 2; cp = b0 & 0x1F; minimum = 0x80; }
        else if (b0 >= 0xE0 && b0 <= 0xEF) { length = 3; cp = b0 & 0x0F; minimum = 0x800; }
        else if (b0 >= 0xF0 && b0 <= 0xF4) { length = 4; cp = b0 & 0x07; minimum = 0x10000; }

        if (length == 0 || i + length > text.size()) {
            ++i;
            return b0;
        }
        for (size_t k = 1; k < length; ++k) {
            const unsigned char b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) {
                ++i;
                return b0;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            ++i;
            return b0;
        }
        i += length;
        return cp;
    }

    inline size_t NextBoundary(const std::string& text, size_t i) {
        if (i >= text.size()) {
            return text.size();
        }
        Next(text, i);
        return i;
    }

    // Start of the codepoint that ends at i.
    inline size_t PrevBoundary(const std::string& text, size_t i) {
        if (i == 0) {
            return 0;
        }
        // A sequence is at most 4 bytes: try the furthest start whose codepoint ends exactly at i.
        for (size_t back = (i < 4 ? i : 4); back > 1; --back) {
            if (NextBoundary(text, i - back) == i) {
                return i - back;
            }
        }
        return i - 1;
    }

    inline std::u32string Decode(const std::string& text) {
        std::u32string result;
        result.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            result.push_back(Next(text, i));
        }
        return result;
    }
}
