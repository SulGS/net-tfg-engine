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
#include <openssl/evp.h>
#include <zstd.h>

using AssetID = uint64_t;

struct AssetLocation {
    uint64_t offset;
    uint64_t size;    // bytes stored in the bin (a Zstd frame)
    uint64_t rawSize; // bytes once decompressed
    uint32_t binId;   // which loaded bin
};

struct BinData {
    std::vector<uint8_t> data;
    std::string name;
    uint64_t dataOffset = 0; // offset to start of actual asset data
};

struct TextureID { GLuint value; };
struct AudioBuffer { ALuint value; };
struct ShaderSource {
    std::string code;
};


struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent;
};

struct SubMeshRange {
    uint32_t indexOffset;   // starting index in EBO
    uint32_t indexCount;    // number of indices
    GLuint   diffuseTex = 0;  // baseColor        (RGBA)
    GLuint   normalTex = 0;  // tangent-space     (RGB)
    GLuint   mrTex = 0;  // G=roughness B=metallic
    GLuint   occlusionTex = 0;  // R=AO
    GLuint   emissiveTex = 0;  // RGB HDR emissive
};

struct MeshBuffer {
    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint EBO = 0;
    std::vector<SubMeshRange> subMeshes; // multi-primitive support
};



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

    bool loadBin(const std::string& binFile)
    {
        // binNameToId survives unloadBin(), so an existing entry means already-loaded (data non-empty) or known-but-unloaded (must reload into the same slot below).
        auto existing = binNameToId.find(binFile);
        if (existing != binNameToId.end() && !bins[existing->second].data.empty())
            return true;

        std::ifstream f(binFile, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return false;

        size_t size = f.tellg();
        f.seekg(0, std::ios::beg);

		if (size < 12) {
			Debug::Error("AssetManager") << "Bin file too small: " << binFile << "\n";
			return false;
		}

        BinData bin;
        bin.data.resize(size);
        bin.name = binFile;
        f.read(reinterpret_cast<char*>(bin.data.data()), size);

        const uint8_t* ptr = bin.data.data();

        if (std::memcmp(ptr, "ASPK", 4) != 0) {
            Debug::Error("AssetManager") << "Invalid bin magic: " << binFile << "\n";
            return false;
        }

        uint32_t version = *reinterpret_cast<const uint32_t*>(ptr + 4);
        if (version != BIN_VERSION) {
            Debug::Error("AssetManager") << "Unsupported bin version " << version << " (expected " << BIN_VERSION << "): " << binFile << "\n";
            return false;
        }

        uint32_t entryCount = *reinterpret_cast<const uint32_t*>(ptr + 8);

        // Each entry: id(8) + offset(8) + storedSize(8) + rawSize(8)
        bin.dataOffset = 12 + static_cast<uint64_t>(entryCount) * (8 + 8 + 8 + 8);
        if (bin.dataOffset > size) {
            Debug::Error("AssetManager") << "Bin file truncated: " << binFile << "\n";
            return false;
        }

        if (existing != binNameToId.end()) {
            // Re-loading a previously unloaded bin: overwrite its reserved
            // slot instead of appending a new one, so its binId (and every
            // assetIndex entry pointing at it) stays valid.
            bins[existing->second] = std::move(bin);
        }
        else {
            uint32_t binId = static_cast<uint32_t>(bins.size());
            binNameToId[binFile] = binId;
            bins.push_back(std::move(bin));
        }

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
                if (entry.binId == binId && entry.refCount > 0)
                {
                    Debug::Error("AssetManager")
                        << "Attempted to unload bin while assets are still referenced: "
                        << binFile << "\n";
                    return;
                }

                if (entry.binId == binId)
                    toErase.push_back(key);
            }
            for (auto& key : toErase)
            {
                auto itDestroyer = destroyers.find(type);
                if (itDestroyer != destroyers.end())
                {
                    itDestroyer->second(typeMap[key].handle);
                }

                typeMap.erase(key);
            }
        }

        // swap with an empty vector to actually release the buffer's capacity (clear() alone doesn't).
        std::vector<uint8_t>().swap(bins[binId].data);

        // Deliberately NOT erasing binNameToId[binFile]: the binId slot
        // stays reserved for this name so a later loadBin() reuses it — see
        // loadBin()'s comment for why a fresh slot would break assetIndex.

        Debug::Info("AssetManager") << "Unloaded bin: " << binFile << "\n";
    }

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
            << ", size: " << loc.rawSize << " (" << loc.size << " compressed)\n";

        if (loc.binId >= bins.size() || bins[loc.binId].data.empty())
        {
            Debug::Error("AssetManager") << "Bin not loaded for asset: " << key << "\n";
            return std::nullopt;
        }

        auto loaderIt = loaders.find(typeid(Handle));
        if (loaderIt == loaders.end()) return std::nullopt;

        std::any anyHandle = loaderIt->second(loc, bins[loc.binId]);

        if (!anyHandle.has_value())
        {
            Debug::Error("AssetManager") << "Loader returned empty handle for: " << key << "\n";
            return std::nullopt;
        }

        Handle h = std::any_cast<Handle>(anyHandle);

        if constexpr (std::is_same_v<Handle, AudioBuffer>)
        {
            if (h.value == 0)
            {
                Debug::Error("AssetManager") << "Invalid AudioBuffer for asset: " << key << "\n";
                return std::nullopt;
            }
        }
        else if constexpr (std::is_same_v<Handle, TextureID>)
        {
            if (h.value == 0)
            {
                Debug::Error("AssetManager") << "Invalid TextureID for asset: " << key << "\n";
                return std::nullopt;
            }
        }

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

    template<typename Handle>
    void registerType(
        std::function<Handle(const uint8_t*, size_t)> loader,
        std::function<void(Handle)> destroyer)
    {
        loaders[typeid(Handle)] = [loader](const AssetLocation& loc, const BinData& bin) {
            const uint64_t available = bin.data.size() - bin.dataOffset;
            if (loc.offset > available || loc.size > available - loc.offset) {
                Debug::Error("AssetManager") << "Asset lies outside its bin: " << bin.name << "\n";
                return std::any();
            }

            const uint8_t* stored = bin.data.data() + bin.dataOffset + loc.offset;

            // Decompress into a scratch buffer that only lives for the loader call (loaders copy what they need: GL upload, AL buffer, string, glTF parse).
            std::vector<uint8_t> raw;
            if (!decompress(stored, static_cast<size_t>(loc.size), static_cast<size_t>(loc.rawSize), raw))
            {
                Debug::Error("AssetManager") << "Zstd decompression failed in bin: " << bin.name << "\n";
                return std::any(); // loadAsset() reports an empty handle
            }
            return std::any(loader(raw.data(), raw.size()));
            };

        destroyers[typeid(Handle)] = [destroyer](std::any h) {
            destroyer(std::any_cast<Handle>(h));
            };
    }

    void setAssetIndex(const std::unordered_map<AssetID, AssetLocation>& idx)
    {
        assetIndex = idx;
    }

    template<typename Handle>
    void clearType()
    {
        auto it = assets.find(typeid(Handle));
        if (it == assets.end()) return;

        for (auto& [id, entry] : it->second) {
            destroy<Handle>(std::any_cast<Handle>(entry.handle));
        }
        it->second.clear();
    }

private:
    AssetManager()
    {
        loadFromBuildRoot();
    }

    static constexpr uint32_t BIN_VERSION = 3;
    static constexpr uint32_t IDX_VERSION = 3;

    static bool decompress(const uint8_t* src, size_t srcSize, size_t rawSize, std::vector<uint8_t>& out)
    {
        out.resize(rawSize);
        size_t written = ZSTD_decompress(out.data(), out.size(), src, srcSize);
        return !ZSTD_isError(written) && written == rawSize;
    }

    static AssetID hashAsset(const std::string& path)
    {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        // MD5() de <openssl/md5.h> esta deprecado desde OpenSSL 3.0; EVP_Digest
        // es la API sustituta para un hash de una sola pasada.
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLen = 0;
        EVP_Digest(normalized.data(), normalized.size(), digest, &digestLen, EVP_md5(), nullptr);

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

        char magic[4] = {};
        uint32_t idxVersion = 0;
        f.read(magic, sizeof(magic));
        f.read(reinterpret_cast<char*>(&idxVersion), sizeof(idxVersion));
        if (!f || std::memcmp(magic, "AIDX", 4) != 0 || idxVersion != IDX_VERSION)
        {
            Debug::Error("AssetManager") << "assets.idx has an unsupported format (expected version " << IDX_VERSION << "); repack the assets with AssetsPackager.py\n";
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
            f.read(reinterpret_cast<char*>(&loc.rawSize), sizeof(loc.rawSize));

            idx[assetID] = loc;
        }

        assetIndex = std::move(idx);

        // Bin table, written by AssetsPackager.py after the entries: the packer's numbering of the bins, by name. Every entry above points at a bin by that number, but loadBin() would hand out slots in the order the game happens to load bins — which only matches by luck (it did with menu + online_level; a third scene, loaded second, took the slot the packer gave to online_level, and its own assets pointed past the end). So reserve each slot by name here; loadBin() then loads into it. An index without the table (older packer) just keeps the load-order behaviour.
        uint32_t binCount = 0;
        if (f.read(reinterpret_cast<char*>(&binCount), sizeof(binCount)))
        {
            for (uint32_t i = 0; i < binCount; ++i)
            {
                uint32_t binId = 0;
                uint16_t nameLen = 0;
                if (!f.read(reinterpret_cast<char*>(&binId), sizeof(binId))) break;
                if (!f.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen))) break;

                std::string name(nameLen, '\0');
                if (nameLen > 0 && !f.read(name.data(), nameLen)) break;

                if (bins.size() <= binId) bins.resize(binId + 1);
                bins[binId].name = name;
                binNameToId[name] = binId;
                Debug::Info("AssetManager") << "Reserved slot " << binId << " for bin: " << name << "\n";
            }
        }

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
        if (it == destroyers.end())
            return;

        it->second(std::any(h));
    }

};
