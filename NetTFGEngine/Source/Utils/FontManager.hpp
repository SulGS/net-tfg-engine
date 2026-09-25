#ifndef FONTMANAGER_HPP
#define FONTMANAGER_HPP

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <map>
#include <string>

struct Character {
    GLuint textureID;   // Glyph texture
    glm::ivec2 size;    // Size of glyph
    glm::ivec2 bearing; // Offset from baseline to left/top of glyph
    GLuint advance;     // Horizontal offset to advance to next glyph
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize);
    // Glyph for a Unicode codepoint; '?' when the font doesn't have it loaded. Text is UTF-8:
    // walk it with Utf8::Next / Utf8::Decode, never byte by byte.
    const Character* GetCharacter(const std::string& fontName, char32_t c) const;
    // A plain char would sign-extend UTF-8 bytes into bogus codepoints; use U'x' literals.
    const Character* GetCharacter(const std::string& fontName, char c) const = delete;
    bool HasFont(const std::string& fontName) const;
    glm::vec2 MeasureText(const std::string& fontName, const std::string& text, float scale = 1.0f) const;

private:
    FT_Library ft;
    std::map<std::string, std::map<char32_t, Character>> fonts;
    
    void GenerateCharacterTexture(FT_Face face, char32_t c, std::map<char32_t, Character>& characters);
};

#endif // FONTMANAGER_HPP