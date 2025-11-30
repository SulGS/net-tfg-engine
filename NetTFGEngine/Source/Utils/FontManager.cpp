#include "FontManager.hpp"
#include "Utils/Debug/Debug.hpp"
#include <iostream>

FontManager::FontManager() {
    if (FT_Init_FreeType(&ft)) {
        std::cerr << "ERROR: Could not init FreeType Library" << std::endl;
    }
}

FontManager::~FontManager() {
    // Delete all character textures
    for (auto& [fontName, characters] : fonts) {
        for (auto& [c, character] : characters) {
            glDeleteTextures(1, &character.textureID);
        }
    }
    
    FT_Done_FreeType(ft);
}

bool FontManager::LoadFont(const std::string& fontName, const std::string& fontPath, unsigned int fontSize) {
    FT_Face face;
    if (FT_New_Face(ft, fontPath.c_str(), 0, &face)) {
        std::cerr << "ERROR: Failed to load font: " << fontPath << std::endl;
        return false;
    }

    // Set font size
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    // Disable byte-alignment restriction
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Load first 128 ASCII characters
    std::map<char, Character> characters;
    for (unsigned char c = 0; c < 128; c++) {
        GenerateCharacterTexture(face, c, characters);
    }

    fonts[fontName] = characters;

    FT_Done_Face(face);
    
    Debug::Info("FontManager") << "Loaded font: " << fontName << " from " << fontPath << "\n";
    return true;
}

void FontManager::GenerateCharacterTexture(FT_Face face, char c, std::map<char, Character>& characters) {
    // Load character glyph
    if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
        std::cerr << "ERROR: Failed to load Glyph for character: " << c << std::endl;
        return;
    }

    // Generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RED,
        face->glyph->bitmap.width,
        face->glyph->bitmap.rows,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        face->glyph->bitmap.buffer
    );

    // Set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Store character
    Character character = {
        texture,
        glm::ivec2(face->glyph->bitmap.width, face->glyph->bitmap.rows),
        glm::ivec2(face->glyph->bitmap_left, face->glyph->bitmap_top),
        static_cast<GLuint>(face->glyph->advance.x)
    };
    
    characters.insert(std::pair<char, Character>(c, character));
}

const Character* FontManager::GetCharacter(const std::string& fontName, char c) const {
    auto fontIt = fonts.find(fontName);
    if (fontIt == fonts.end()) {
        return nullptr;
    }

    auto charIt = fontIt->second.find(c);
    if (charIt == fontIt->second.end()) {
        return nullptr;
    }

    return &charIt->second;
}

bool FontManager::HasFont(const std::string& fontName) const {
    return fonts.find(fontName) != fonts.end();
}

glm::vec2 FontManager::MeasureText(const std::string& fontName, const std::string& text, float scale) const {
    auto fontIt = fonts.find(fontName);
    if (fontIt == fonts.end()) {
        return glm::vec2(0.0f);
    }

    float width = 0.0f;
    float maxHeight = 0.0f;

    for (char c : text) {
        auto charIt = fontIt->second.find(c);
        if (charIt != fontIt->second.end()) {
            const Character& ch = charIt->second;
            width += (ch.advance >> 6) * scale;
            maxHeight = std::max(maxHeight, static_cast<float>(ch.size.y) * scale);
        }
    }

    return glm::vec2(width, maxHeight);
}