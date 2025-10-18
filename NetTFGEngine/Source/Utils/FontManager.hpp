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

    // Load a font from file
    bool LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize);
    
    // Get character info for a specific font
    const Character* GetCharacter(const std::string& fontName, char c) const;
    
    // Check if font exists
    bool HasFont(const std::string& fontName) const;
    
    // Measure text size
    glm::vec2 MeasureText(const std::string& fontName, const std::string& text, float scale = 1.0f) const;

private:
    FT_Library ft;
    std::map<std::string, std::map<char, Character>> fonts;
    
    void GenerateCharacterTexture(FT_Face face, char c, std::map<char, Character>& characters);
};

#endif // FONTMANAGER_HPP