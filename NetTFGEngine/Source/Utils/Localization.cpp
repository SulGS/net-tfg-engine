#include "Utils/Localization.hpp"
#include "Utils/Debug/Debug.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{
    struct CsvRow {
        std::vector<std::string> fields;
        size_t line = 0;  // 1-based line where the row starts, for messages
    };

    std::string Trim(const std::string& s) {
        size_t start = 0;
        size_t end = s.size();
        while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        return s.substr(start, end - start);
    }

    bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    // ';' when the header line has more of them than ',' outside quotes, ',' otherwise.
    char DetectSeparator(const std::string& text, size_t start) {
        size_t commas = 0;
        size_t semicolons = 0;
        bool inQuotes = false;
        for (size_t i = start; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '"') inQuotes = !inQuotes;
            else if (!inQuotes && (c == '\n' || c == '\r')) break;
            else if (!inQuotes && c == ',') ++commas;
            else if (!inQuotes && c == ';') ++semicolons;
        }
        return semicolons > commas ? ';' : ',';
    }

    // RFC 4180. Blank lines are skipped. False (with errorLine set) on an unterminated quote.
    bool ParseCsv(const std::string& text, size_t start, char separator, std::vector<CsvRow>& rows, size_t& errorLine) {
        CsvRow row;
        std::string field;
        bool inQuotes = false;
        bool fieldQuoted = false;
        size_t line = 1;
        row.line = line;

        auto endField = [&]() {
            row.fields.push_back(std::move(field));
            field.clear();
            fieldQuoted = false;
        };
        auto endRow = [&]() {
            endField();
            const bool blank = row.fields.size() == 1 && row.fields[0].empty();
            if (!blank) {
                rows.push_back(std::move(row));
            }
            row = CsvRow{};
            row.line = line;
        };

        for (size_t i = start; i < text.size(); ++i) {
            const char c = text[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        field.push_back('"');
                        ++i;
                    }
                    else {
                        inQuotes = false;
                    }
                }
                else {
                    if (c == '\n') ++line;
                    field.push_back(c);
                }
            }
            else if (c == '"' && field.empty() && !fieldQuoted) {
                inQuotes = true;
                fieldQuoted = true;
            }
            else if (c == separator) {
                endField();
            }
            else if (c == '\r' || c == '\n') {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                ++line;
                endRow();
            }
            else {
                field.push_back(c);
            }
        }

        if (inQuotes) {
            errorLine = row.line;
            return false;
        }
        if (!field.empty() || fieldQuoted || !row.fields.empty()) {
            endRow();
        }
        return true;
    }
}

Localization& Localization::Get() {
    static Localization instance;
    return instance;
}

bool Localization::LoadCSVFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Debug::Error("Localization") << "Could not open " << path << "\n";
        return false;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return LoadCSVString(contents.str(), path);
}

bool Localization::LoadCSVString(const std::string& csv, const std::string& sourceName) {
    // UTF-8 BOM, which Excel writes when saving "CSV UTF-8".
    size_t start = 0;
    if (csv.size() >= 3 && static_cast<unsigned char>(csv[0]) == 0xEF
        && static_cast<unsigned char>(csv[1]) == 0xBB && static_cast<unsigned char>(csv[2]) == 0xBF) {
        start = 3;
    }

    std::vector<CsvRow> rows;
    size_t errorLine = 0;
    if (!ParseCsv(csv, start, DetectSeparator(csv, start), rows, errorLine)) {
        Debug::Error("Localization") << sourceName << ": unterminated quoted field starting at line "
            << errorLine << "\n";
        return false;
    }
    if (rows.empty()) {
        Debug::Error("Localization") << sourceName << ": empty file\n";
        return false;
    }

    const std::vector<std::string>& header = rows[0].fields;
    if (!EqualsIgnoreCase(Trim(header[0]), "Key")) {
        Debug::Error("Localization") << sourceName << ": the first column must be \"Key\"\n";
        return false;
    }

    // Column index -> language; an empty name means the column is ignored.
    std::vector<std::string> columns(header.size());
    for (size_t col = 1; col < header.size(); ++col) {
        const std::string language = Trim(header[col]);
        if (language.empty()) {
            Debug::Warning("Localization") << sourceName << ": column " << col + 1
                << " has no language name, ignored\n";
            continue;
        }
        if (std::find(columns.begin(), columns.end(), language) != columns.end()) {
            Debug::Error("Localization") << sourceName << ": language \"" << language << "\" appears twice\n";
            return false;
        }
        columns[col] = language;
    }

    std::unique_lock lock(mutex_);

    for (const std::string& language : columns) {
        if (!language.empty() && std::find(languages_.begin(), languages_.end(), language) == languages_.end()) {
            languages_.push_back(language);
            texts_[language];
        }
    }

    std::unordered_set<std::string> seenKeys;
    size_t loadedKeys = 0;
    for (size_t r = 1; r < rows.size(); ++r) {
        const CsvRow& row = rows[r];
        const std::string key = Trim(row.fields[0]);
        // Rows without a key, or starting with '#', are for people editing the file.
        if (key.empty() || key[0] == '#') {
            continue;
        }
        if (!seenKeys.insert(key).second) {
            Debug::Warning("Localization") << sourceName << ":" << row.line << ": key \"" << key
                << "\" repeated, the later row wins\n";
        }
        if (row.fields.size() > header.size()) {
            Debug::Warning("Localization") << sourceName << ":" << row.line << ": row has "
                << row.fields.size() << " cells but the header " << header.size() << ", extra cells ignored\n";
        }

        const size_t cells = std::min(row.fields.size(), header.size());
        for (size_t col = 1; col < cells; ++col) {
            // An empty cell is a missing translation: it falls back instead of showing nothing.
            if (!columns[col].empty() && !row.fields[col].empty()) {
                texts_[columns[col]][key] = row.fields[col];
            }
        }
        ++loadedKeys;
    }

    if (language_.empty() && !languages_.empty()) {
        language_ = languages_.front();
    }

    {
        std::lock_guard missingLock(missingMutex_);
        reportedMissing_.clear();  // a new file may fill keys that were missing
    }

    Debug::Info("Localization") << "Loaded " << loadedKeys << " keys from " << sourceName << "\n";
    return true;
}

bool Localization::SetLanguage(const std::string& language) {
    std::unique_lock lock(mutex_);
    if (texts_.find(language) == texts_.end()) {
        Debug::Warning("Localization") << "Unknown language \"" << language << "\", keeping \""
            << language_ << "\"\n";
        return false;
    }
    language_ = language;
    return true;
}

std::string Localization::GetLanguage() const {
    std::shared_lock lock(mutex_);
    return language_;
}

void Localization::SetFallbackLanguage(const std::string& language) {
    std::unique_lock lock(mutex_);
    fallbackLanguage_ = language;
}

std::string Localization::GetFallbackLanguage() const {
    std::shared_lock lock(mutex_);
    return fallbackLanguage_;
}

std::vector<std::string> Localization::GetLanguages() const {
    std::shared_lock lock(mutex_);
    return languages_;
}

bool Localization::HasLanguage(const std::string& language) const {
    std::shared_lock lock(mutex_);
    return texts_.find(language) != texts_.end();
}

std::string Localization::GetText(const std::string& key) const {
    std::shared_lock lock(mutex_);
    return Lookup(key, language_);
}

std::string Localization::GetText(const std::string& key, const std::string& language) const {
    std::shared_lock lock(mutex_);
    return Lookup(key, language);
}

bool Localization::HasText(const std::string& key, const std::string& language) const {
    std::shared_lock lock(mutex_);
    return Find(key, language) != nullptr;
}

void Localization::Clear() {
    std::unique_lock lock(mutex_);
    languages_.clear();
    texts_.clear();
    language_.clear();
    fallbackLanguage_.clear();

    std::lock_guard missingLock(missingMutex_);
    reportedMissing_.clear();
}

const std::string* Localization::Find(const std::string& key, const std::string& language) const {
    auto languageIt = texts_.find(language);
    if (languageIt == texts_.end()) {
        return nullptr;
    }
    auto textIt = languageIt->second.find(key);
    return textIt == languageIt->second.end() ? nullptr : &textIt->second;
}

std::string Localization::Lookup(const std::string& key, const std::string& language) const {
    if (const std::string* text = Find(key, language)) {
        return *text;
    }
    if (!fallbackLanguage_.empty() && fallbackLanguage_ != language) {
        if (const std::string* text = Find(key, fallbackLanguage_)) {
            return *text;
        }
    }

    std::lock_guard missingLock(missingMutex_);
    if (reportedMissing_.insert(language + '\n' + key).second) {
        Debug::Warning("Localization") << "No text for \"" << key << "\" in \"" << language << "\"\n";
    }
    return key;
}
