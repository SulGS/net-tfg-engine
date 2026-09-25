#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Translation tables loaded from CSV files:
//
//   Key,es,en
//   menu.search,Buscar partida,Find match
//   pause.question,"¿Estás seguro de querer salir?","Are you sure you want to quit?"
//
// The first column must be "Key"; every other header is a language name. Files are UTF-8 (a BOM
// is skipped), fields follow RFC 4180 (quotes, "" escapes, line breaks inside quotes) and the
// separator is ',' or ';' (Excel with a Spanish locale saves ';'), detected from the header.
// Loading several files merges them: a key/language already loaded is overwritten.
//
// Lookups use the current language (SetLanguage). A key with no text there falls back to the
// fallback language, and then to the key itself, so a missing translation shows up on screen
// instead of an empty label. Thread safe: loads and lookups may come from any thread.
class Localization
{
public:
    static Localization& Get();

    Localization(const Localization&) = delete;
    Localization& operator=(const Localization&) = delete;

    // Rows already parsed before an error are kept; false and a logged error on failure.
    bool LoadCSVFile(const std::string& path);
    // Same as LoadCSVFile for text already in memory (e.g. read from an asset bin).
    // sourceName only labels the log messages.
    bool LoadCSVString(const std::string& csv, const std::string& sourceName = "<memory>");

    // Language used by GetText(key). Must be one of the loaded columns; otherwise returns false
    // and keeps the current one. The first language loaded becomes the current one if none is set.
    bool SetLanguage(const std::string& language);
    std::string GetLanguage() const;

    // Language tried when the current one has no text for a key. Empty = no fallback.
    void SetFallbackLanguage(const std::string& language);
    std::string GetFallbackLanguage() const;

    // Every language column loaded so far, in the order first seen.
    std::vector<std::string> GetLanguages() const;
    bool HasLanguage(const std::string& language) const;

    // Text of `key` in the current language (with the fallbacks described above).
    std::string GetText(const std::string& key) const;
    // Text of `key` in a specific language, same fallbacks.
    std::string GetText(const std::string& key, const std::string& language) const;
    // True if `key` has a non-empty text in `language` (no fallbacks).
    bool HasText(const std::string& key, const std::string& language) const;

    // Drops every table and language; the current and fallback languages are reset.
    void Clear();

private:
    Localization() = default;

    // Caller holds the lock (shared is enough).
    const std::string* Find(const std::string& key, const std::string& language) const;
    std::string Lookup(const std::string& key, const std::string& language) const;

    mutable std::shared_mutex mutex_;
    std::vector<std::string> languages_;
    // language -> key -> text
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> texts_;
    std::string language_;
    std::string fallbackLanguage_;

    // Keys already reported as missing, so a label looked up every frame logs only once.
    mutable std::mutex missingMutex_;
    mutable std::unordered_set<std::string> reportedMissing_;
};

// Shorthand for Localization::Get().GetText(key).
inline std::string Tr(const std::string& key) {
    return Localization::Get().GetText(key);
}
