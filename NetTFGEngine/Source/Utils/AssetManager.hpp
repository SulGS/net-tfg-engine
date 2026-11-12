#pragma once

#ifdef NETTFG_HEADLESS
#error "Utils/AssetManager.hpp is client-only (OpenGL/GLFW/OpenAL/FreeType/SOIL): headless targets (NetTFGEngineCore, GameServer) must not include it"
#endif
#include <unordered_map>
#include <string>
#include <typeindex>
#include <optional>
#include <functional>
#include <any>
#include <fstream>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <future>
#include <thread>
#include <set>
#include <chrono>
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
struct AudioBuffer { ALuint value; };   // mono (downmixed): for positional sources
// Same WAV with its channels kept (mono or stereo): for non-positional playback such as music. A separate type, so a
// file can be loaded both ways without one handle overwriting the other.
struct MusicBuffer { ALuint value; };
struct ShaderSource {
    std::string code;
};
// The asset's file bytes, undecoded, for loaders that parse a format themselves (e.g. cube map faces decoded to float
// by RenderSystem: the TextureID path goes through SOIL straight to an 8-bit, DXT-compressed texture).
struct BlobData {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
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
    // Local-space bounding sphere of the vertices (for culling, e.g. point light shadow casters). Radius < 0 = unknown:
    // never culled.
    glm::vec3 boundsCenter{ 0.0f };
    float     boundsRadius = -1.0f;
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

    // Safe from any thread: the file is read and validated without the lock (the online bin is >100 MB, and the render
    // and audio threads keep loading assets meanwhile), then installed under it.
    bool loadBin(const std::string& binFile)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            // binNameToId survives unloadBin(), so an existing entry means already-loaded (data non-empty) or known-but-unloaded (must reload into the same slot below).
            auto existing = binNameToId.find(binFile);
            if (existing != binNameToId.end() && !bins[existing->second].data.empty())
                return true;
        }

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

        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // Looked up again: another thread may have loaded or reserved this bin while the file was being read.
        auto existing = binNameToId.find(binFile);
        if (existing != binNameToId.end() && !bins[existing->second].data.empty())
            return true;

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
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto itBin = binNameToId.find(binFile);
        if (itBin == binNameToId.end()) return;

        if (residentBins_.count(binFile)) {
            Debug::Info("AssetManager") << "Kept resident bin loaded: " << binFile << "\n";
            return;
        }

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
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        AssetID id = hashAsset(key);

        auto itLoaded = assets[typeid(Handle)].find(id);
        if (itLoaded != assets[typeid(Handle)].end())
        {
            ++itLoaded->second.refCount;
            return std::any_cast<Handle>(itLoaded->second.handle);
        }

        // Prefetched (or being prefetched): wait for its CPU half without holding the lock (the audio thread keeps
        // loading meanwhile), then only the finishing step (GL upload) runs here.
        auto itPending = pending_.find({ std::type_index(typeid(Handle)), id });
        if (itPending != pending_.end())
        {
            std::shared_future<std::any> future = itPending->second;
            lock.unlock();
            const auto waitStart = std::chrono::steady_clock::now();
            std::any prepared = future.get();
            const double waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
            lock.lock();
            pending_.erase({ std::type_index(typeid(Handle)), id });

            // Another thread may have finished it while this one waited.
            auto itNow = assets[typeid(Handle)].find(id);
            if (itNow != assets[typeid(Handle)].end())
            {
                ++itNow->second.refCount;
                return std::any_cast<Handle>(itNow->second.handle);
            }

            auto finisherIt = finishers.find(typeid(Handle));
            auto itIndex = assetIndex.find(id);
            if (prepared.has_value() && finisherIt != finishers.end() && itIndex != assetIndex.end())
            {
                const auto finishStart = std::chrono::steady_clock::now();
                std::any anyHandle = finisherIt->second(prepared);
                prepared.reset();
                if (anyHandle.has_value())
                {
                    Entry entry;
                    entry.handle = anyHandle;
                    entry.refCount = 1;
                    entry.binId = itIndex->second.binId;
                    assets[typeid(Handle)][id] = std::move(entry);
                    Debug::Info("AssetManager") << "Loaded prefetched asset: " << key << " (waited " << waitedMs
                        << " ms for the worker, upload "
                        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - finishStart).count()
                        << " ms)\n";
                    return std::any_cast<Handle>(anyHandle);
                }
            }
            Debug::Warning("AssetManager") << "Prefetch of " << key << " gave nothing, loading it normally\n";
        }

        auto& typeMap = assets[typeid(Handle)];

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

        if constexpr (std::is_same_v<Handle, AudioBuffer> || std::is_same_v<Handle, MusicBuffer>)
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
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto& typeMap = assets[typeid(Handle)];
        auto it = typeMap.find(hashAsset(key));
        if (it == typeMap.end()) return;

        Entry& entry = it->second;
        assert(entry.refCount > 0);
        --entry.refCount;

        // A resident bin's assets stay loaded at zero references, so the next match finds them ready.
        if (entry.refCount == 0 && entry.binId < bins.size() && residentBins_.count(bins[entry.binId].name))
            return;

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
        std::lock_guard<std::recursive_mutex> lock(mutex_);
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

    // A type whose loader splits in two: `prepare` (CPU only, any thread) and `finish` (needs the GL/AL context). Loaded
    // normally it runs both in a row; prefetch<Handle>() runs `prepare` early on a worker thread.
    template<typename Handle, typename Prepared>
    void registerTwoPhaseType(
        std::function<Prepared(const uint8_t*, size_t)> prepare,
        std::function<Handle(Prepared&)> finish,
        std::function<void(Handle)> destroyer)
    {
        registerType<Handle>(
            [prepare, finish](const uint8_t* data, size_t size) -> Handle {
                Prepared prepared = prepare(data, size);
                return finish(prepared);
            },
            destroyer);

        std::lock_guard<std::recursive_mutex> lock(mutex_);
        rawPreparers[typeid(Handle)] = [prepare](const uint8_t* data, size_t size) -> std::any {
            return std::any(std::make_shared<Prepared>(prepare(data, size)));
            };
        finishers[typeid(Handle)] = [finish](std::any& prepared) -> std::any {
            auto p = std::any_cast<std::shared_ptr<Prepared>>(prepared);
            return std::any(finish(*p));
            };
    }

    // Starts the CPU half of these assets (registerTwoPhaseType types only) on one worker thread, in order, so a later
    // loadAsset on the render thread only has to upload. Assets already loaded or prefetching are skipped; their bin
    // must be loaded. Returns how many were queued.
    template<typename Handle>
    size_t prefetch(const std::vector<Key>& keys)
    {
        struct Job { Key key; std::vector<uint8_t> stored; uint64_t rawSize; std::shared_ptr<std::promise<std::any>> promise; };
        std::vector<Job> jobs;
        std::function<std::any(const uint8_t*, size_t)> preparer;

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto prepIt = rawPreparers.find(typeid(Handle));
            if (prepIt == rawPreparers.end()) return 0;
            preparer = prepIt->second;

            for (const Key& key : keys) {
                const AssetID id = hashAsset(key);
                if (assets[typeid(Handle)].count(id) || pending_.count({ std::type_index(typeid(Handle)), id })) continue;

                auto itIndex = assetIndex.find(id);
                if (itIndex == assetIndex.end()) continue;
                const AssetLocation& loc = itIndex->second;
                if (loc.binId >= bins.size() || bins[loc.binId].data.empty()) continue;
                const BinData& bin = bins[loc.binId];
                const uint64_t available = bin.data.size() - bin.dataOffset;
                if (loc.offset > available || loc.size > available - loc.offset) continue;

                // A copy of the compressed bytes: the worker then needs nothing from the manager.
                const uint8_t* stored = bin.data.data() + bin.dataOffset + loc.offset;
                Job job{ key, std::vector<uint8_t>(stored, stored + loc.size), loc.rawSize, std::make_shared<std::promise<std::any>>() };
                pending_[{ std::type_index(typeid(Handle)), id }] = job.promise->get_future().share();
                jobs.push_back(std::move(job));
            }
        }

        if (jobs.empty()) return 0;

        const size_t count = jobs.size();
        std::thread([jobs = std::move(jobs), preparer]() mutable {
            Debug::SetThreadName("AssetPrefetch");
            for (Job& job : jobs) {
                const auto start = std::chrono::steady_clock::now();
                std::any result;
                try {
                    std::vector<uint8_t> raw;
                    if (decompress(job.stored.data(), job.stored.size(), static_cast<size_t>(job.rawSize), raw)) {
                        std::vector<uint8_t>().swap(job.stored);
                        result = preparer(raw.data(), raw.size());
                    }
                    else {
                        Debug::Error("AssetManager") << "Prefetch: Zstd decompression failed for " << job.key << "\n";
                    }
                }
                catch (const std::exception& e) {
                    Debug::Error("AssetManager") << "Prefetch of " << job.key << " failed: " << e.what() << "\n";
                    result.reset();
                }
                Debug::Info("AssetManager") << "Prefetched " << job.key << " in "
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms\n";
                job.promise->set_value(std::move(result));
            }
            }).detach();

        return count;
    }

    // Prefetches (any type) whose worker hasn't finished the CPU half yet. A load step polls this rather than calling
    // loadAsset, which would block the render thread (and its window's message pump) until the worker is done.
    size_t prefetchesInFlight() const
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& [key, future] : pending_)
            if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) ++count;
        return count;
    }

    // Resident bins are never unloaded and their assets stay loaded at zero references: the next scene that uses them
    // (e.g. the next match) finds them ready instead of decoding and uploading everything again.
    void setKeepResident(const std::string& binFile, bool keep)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (keep) residentBins_.insert(binFile);
        else residentBins_.erase(binFile);
    }

    bool isResident(const std::string& binFile) const
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return residentBins_.count(binFile) != 0;
    }

    void setAssetIndex(const std::unordered_map<AssetID, AssetLocation>& idx)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        assetIndex = idx;
    }

    template<typename Handle>
    void clearType()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
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

    // Guards every member below. Used from the render thread (meshes, textures, shaders, music, unloadBin), the audio
    // thread (SFX buffers) and the client-activation thread (loadBin); unloadBin() walking the AudioBuffer map while the
    // audio thread inserted into it was a crash/hang during the switch into the online level. Leaf lock: loaders and
    // destroyers only call GL/AL/logging, never back into code that takes the EntityManager or audio mutex.
    mutable std::recursive_mutex mutex_;

    std::vector<BinData> bins;
    std::unordered_map<std::string, uint32_t> binNameToId;

    std::unordered_map<std::type_index, std::unordered_map<AssetID, Entry>> assets;
    std::unordered_map<std::type_index, std::function<std::any(const AssetLocation&, const BinData&)>> loaders;
    std::unordered_map<std::type_index, std::function<void(std::any)>> destroyers;
    std::unordered_map<AssetID, AssetLocation> assetIndex;

    // registerTwoPhaseType: the CPU half (on decompressed bytes) and the finishing half, per type.
    std::unordered_map<std::type_index, std::function<std::any(const uint8_t*, size_t)>> rawPreparers;
    std::unordered_map<std::type_index, std::function<std::any(std::any&)>> finishers;

    struct PendingKeyHash {
        size_t operator()(const std::pair<std::type_index, AssetID>& k) const {
            return k.first.hash_code() ^ static_cast<size_t>(k.second * 0x9E3779B97F4A7C15ULL);
        }
    };
    // Prefetches not yet picked up by loadAsset: the worker's result (a Prepared, or empty on failure).
    std::unordered_map<std::pair<std::type_index, AssetID>, std::shared_future<std::any>, PendingKeyHash> pending_;

    std::set<std::string> residentBins_;

    template<typename Handle>
    void destroy(Handle h)
    {
        auto it = destroyers.find(typeid(Handle));
        if (it == destroyers.end())
            return;

        it->second(std::any(h));
    }

};
