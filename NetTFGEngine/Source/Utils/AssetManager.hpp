#pragma once
#include <unordered_map>
#include <string>
#include <typeindex>
#include <optional>
#include <functional>
#include <any>
#include <fstream>
#include <vector>
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include "Utils/Debug/Debug.hpp"

#include <AL/al.h>
#include <openssl/md5.h>

using AssetID = uint64_t;

struct AssetLocation {
    uint64_t offset;
    uint64_t size;
    uint32_t binId; // which loaded bin
};

struct BinData {
    std::vector<uint8_t> data;
    std::string name;
    uint64_t dataOffset = 0; // offset to start of actual asset data
};

struct TextureID { GLuint value; };
struct AudioBuffer { ALuint value; };

class AssetManager
{
public:
    using Key = std::string;

    static AssetManager& instance()
    {
        static AssetManager inst;
        return inst;
    }

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /* ============================
       Bin management
       ============================ */
    bool loadBin(const std::string& binFile)
    {
        if (binNameToId.count(binFile)) return true;

        std::ifstream f(binFile, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return false;

        size_t size = f.tellg();
        f.seekg(0, std::ios::beg);

        BinData bin;
        bin.data.resize(size);
        bin.name = binFile;
        f.read(reinterpret_cast<char*>(bin.data.data()), size);

        const uint8_t* ptr = bin.data.data();

        // Validate header
        if (std::memcmp(ptr, "ASPK", 4) != 0) {
            Debug::Error("AssetManager") << "Invalid bin magic: " << binFile << "\n";
            return false;
        }

        uint32_t version = *reinterpret_cast<const uint32_t*>(ptr + 4);
        if (version != 1) {
            Debug::Error("AssetManager") << "Unsupported bin version\n";
            return false;
        }

        uint32_t entryCount = *reinterpret_cast<const uint32_t*>(ptr + 8);

        // Each entry: id(8) + offset(8) + size(8)
        bin.dataOffset = 12 + entryCount * (8 + 8 + 8);

        uint32_t binId = static_cast<uint32_t>(bins.size());
        binNameToId[binFile] = binId;
        bins.push_back(std::move(bin));

        Debug::Info("AssetManager") << "Loaded bin: " << binFile << "\n";
        return true;
    }

    void unloadBin(const std::string& binFile)
    {
        auto itBin = binNameToId.find(binFile);
        if (itBin == binNameToId.end()) return;

        uint32_t binId = itBin->second;

        // Remove all assets referencing this bin
        for (auto& [type, typeMap] : assets)
        {
            std::vector<AssetID> toErase;
            for (auto& [key, entry] : typeMap)
            {
                if (entry.binId == binId)
                    toErase.push_back(key);
            }
            for (auto& key : toErase)
            {
                destroyers[type](typeMap[key].handle);
                typeMap.erase(key);
            }
        }

        bins[binId].data.clear();
        binNameToId.erase(itBin);

        Debug::Info("AssetManager") << "Unloaded bin: " << binFile << "\n";
    }

    /* ============================
       Asset-level API
       ============================ */
    template<typename Handle>
    std::optional<Handle> loadAsset(const Key& key)
    {
        AssetID id = hashAsset(key);

        auto& typeMap = assets[typeid(Handle)];

        auto itLoaded = typeMap.find(id);
        if (itLoaded != typeMap.end())
        {
            ++itLoaded->second.refCount;
            return std::any_cast<Handle>(itLoaded->second.handle);
        }

        auto itIndex = assetIndex.find(id);
        if (itIndex == assetIndex.end())
            return std::nullopt;

        const AssetLocation& loc = itIndex->second;

        Debug::Info("AssetManager") << "Loading asset: " << key
            << " as type " << typeid(Handle).name()
            << ", size: " << loc.size << "\n";

        if (loc.binId >= bins.size() || bins[loc.binId].data.empty())
        {
            Debug::Error("AssetManager") << "Bin not loaded for asset: " << key << "\n";
            return std::nullopt;
        }

        // Load asset via registered loader
        auto loaderIt = loaders.find(typeid(Handle));
        if (loaderIt == loaders.end()) return std::nullopt;

        Handle h = std::any_cast<Handle>(loaderIt->second(loc, bins[loc.binId]));

        Entry entry;
        entry.handle = h;
        entry.refCount = 1;
        entry.binId = loc.binId;
        typeMap[id] = std::move(entry);

        Debug::Info("AssetManager") << "Loaded asset: " << key << " from bin " << bins[loc.binId].name << "\n";
        return h;
    }

    template<typename Handle>
    void unloadAsset(const Key& key)
    {
        auto& typeMap = assets[typeid(Handle)];
        auto it = typeMap.find(hashAsset(key));
        if (it == typeMap.end()) return;

        Entry& entry = it->second;
        assert(entry.refCount > 0);
        --entry.refCount;

        if (entry.refCount == 0)
        {
            destroy<Handle>(std::any_cast<Handle>(entry.handle));
            typeMap.erase(it);
            Debug::Info("AssetManager") << "Unloaded asset: " << key << "\n";
        }
    }

    /* ============================
       Type registration
       ============================ */
    template<typename Handle>
    void registerType(
        std::function<Handle(const uint8_t*, size_t)> loader,
        std::function<void(Handle)> destroyer)
    {
        loaders[typeid(Handle)] = [loader](const AssetLocation& loc, const BinData& bin) {
            return std::any(
                loader(
                    bin.data.data() + bin.dataOffset + loc.offset, // <-- FIXED
                    static_cast<size_t>(loc.size)
                )
            );
            };

        destroyers[typeid(Handle)] = [destroyer](std::any h) {
            destroyer(std::any_cast<Handle>(h));
            };
    }

    /* ============================
       Asset index management
       ============================ */
    void setAssetIndex(const std::unordered_map<AssetID, AssetLocation>& idx)
    {
        assetIndex = idx;
    }

private:
    AssetManager()
    {
        loadFromBuildRoot();
    }

    static AssetID hashAsset(const std::string& path)
    {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(normalized.data()),
            normalized.size(),
            digest);

        AssetID id = 0;
        std::memcpy(&id, digest, sizeof(AssetID)); // first 8 bytes
        return id;
    }

    bool loadFromBuildRoot()
    {
        std::ifstream f("assets.idx", std::ios::binary);
        if (!f.is_open())
        {
            Debug::Error("AssetManager") << "Failed to open assets.idx\n";
            return false;
        }

        uint32_t numAssets = 0;
        f.read(reinterpret_cast<char*>(&numAssets), sizeof(numAssets));

        std::unordered_map<AssetID, AssetLocation> idx;

        for (uint32_t i = 0; i < numAssets; ++i)
        {
            AssetID assetID;
            AssetLocation loc;

            f.read(reinterpret_cast<char*>(&assetID), sizeof(assetID));
            f.read(reinterpret_cast<char*>(&loc.binId), sizeof(loc.binId));
            f.read(reinterpret_cast<char*>(&loc.offset), sizeof(loc.offset));
            f.read(reinterpret_cast<char*>(&loc.size), sizeof(loc.size));

            idx[assetID] = loc;
        }

        assetIndex = std::move(idx);

        Debug::Info("AssetManager")
            << "Loaded asset index with " << numAssets << " entries\n";

        if (std::filesystem::exists("shared.bin"))
            loadBin("shared.bin");

        return true;
    }

    struct Entry {
        std::any handle;
        size_t refCount = 0;
        uint32_t binId = 0;
    };

    std::vector<BinData> bins;
    std::unordered_map<std::string, uint32_t> binNameToId;

    std::unordered_map<std::type_index, std::unordered_map<AssetID, Entry>> assets;
    std::unordered_map<std::type_index, std::function<std::any(const AssetLocation&, const BinData&)>> loaders;
    std::unordered_map<std::type_index, std::function<void(std::any)>> destroyers;
    std::unordered_map<AssetID, AssetLocation> assetIndex;

    template<typename Handle>
    void destroy(Handle h)
    {
        auto it = destroyers.find(typeid(Handle));
        assert(it != destroyers.end());
        it->second(std::any(h));
    }
};
