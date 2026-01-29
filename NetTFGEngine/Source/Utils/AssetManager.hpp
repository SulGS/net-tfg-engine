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
#include "Utils/Debug/Debug.hpp"

#include <AL/al.h>



struct AssetLocation {
    uint64_t offset;
    uint64_t size;
    uint32_t binId; // which loaded bin
};

struct BinData {
    std::vector<uint8_t> data;
    std::string name;
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
        if (binNameToId.count(binFile)) return true; // already loaded

        std::ifstream f(binFile, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return false;

        size_t size = f.tellg();
        f.seekg(0, std::ios::beg);

        BinData bin;
        bin.data.resize(size);
        bin.name = binFile;
        f.read(reinterpret_cast<char*>(bin.data.data()), size);

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
            std::vector<std::string> toErase;
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
        auto& typeMap = assets[typeid(Handle)];

        // Already loaded?
        auto itLoaded = typeMap.find(key);
        if (itLoaded != typeMap.end())
        {
            ++itLoaded->second.refCount;
            return std::any_cast<Handle>(itLoaded->second.handle);
        }

        // Find asset in index
        auto itIndex = assetIndex.find(key);
        if (itIndex == assetIndex.end()) return std::nullopt;

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
        typeMap[key] = std::move(entry);

        Debug::Info("AssetManager") << "Loaded asset: " << key << " from bin " << bins[loc.binId].name << "\n";
        return h;
    }

    template<typename Handle>
    void unloadAsset(const Key& key)
    {
        auto& typeMap = assets[typeid(Handle)];
        auto it = typeMap.find(key);
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
            return std::any(loader(bin.data.data() + loc.offset, static_cast<size_t>(loc.size)));
            };

        destroyers[typeid(Handle)] = [destroyer](std::any h) {
            destroyer(std::any_cast<Handle>(h));
            };
    }

    /* ============================
       Asset index management
       ============================ */
    void setAssetIndex(const std::unordered_map<std::string, AssetLocation>& idx)
    {
        assetIndex = idx;
    }

private:
    AssetManager()
    {
        loadFromBuildRoot();
    }

    bool loadFromBuildRoot()
    {
        namespace fs = std::filesystem;

        std::string indexPath = "assets.idx";
        std::ifstream f(indexPath, std::ios::binary);
        if (!f.is_open()) {
            Debug::Error("AssetManager") << "Failed to open asset index: " << indexPath << "\n";
            return false;
        }

        // Read asset count
        uint32_t numAssets = 0;
        f.read(reinterpret_cast<char*>(&numAssets), sizeof(numAssets));

        std::unordered_map<std::string, AssetLocation> idx;

        // Read asset entries
        for (uint32_t i = 0; i < numAssets; ++i)
        {
            uint32_t nameLen;
            f.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
            std::string name(nameLen, '\0');
            f.read(name.data(), nameLen);

            AssetLocation loc;
            f.read(reinterpret_cast<char*>(&loc.binId), sizeof(loc.binId));
            f.read(reinterpret_cast<char*>(&loc.offset), sizeof(loc.offset));
            f.read(reinterpret_cast<char*>(&loc.size), sizeof(loc.size));

            idx[name] = loc;
        }

        setAssetIndex(idx);
        Debug::Info("AssetManager") << "Loaded asset index with " << numAssets << " entries\n";

        // Load shared.bin if exists
        std::string sharedBinPath = "shared.bin";
        if (fs::exists(sharedBinPath))
        {
            if (loadBin(sharedBinPath))
                Debug::Info("AssetManager") << "Loaded shared bin: " << sharedBinPath << "\n";
            else
                Debug::Error("AssetManager") << "Failed to load shared bin: " << sharedBinPath << "\n";
        }

        return true;
    }

    struct Entry {
        std::any handle;
        size_t refCount = 0;
        uint32_t binId = 0;
    };

    std::vector<BinData> bins;
    std::unordered_map<std::string, uint32_t> binNameToId;

    std::unordered_map<std::type_index, std::unordered_map<std::string, Entry>> assets;
    std::unordered_map<std::type_index, std::function<std::any(const AssetLocation&, const BinData&)>> loaders;
    std::unordered_map<std::type_index, std::function<void(std::any)>> destroyers;
    std::unordered_map<std::string, AssetLocation> assetIndex;

    template<typename Handle>
    void destroy(Handle h)
    {
        auto it = destroyers.find(typeid(Handle));
        assert(it != destroyers.end());
        it->second(std::any(h));
    }
};
