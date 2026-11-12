#pragma once

#ifdef NETTFG_HEADLESS
#error "NetTFG_Engine.hpp is client-only (OpenGL/GLFW/OpenAL/FreeType/SOIL): headless targets (NetTFGEngineCore, GameServer) must not include it"
#endif
#include <unordered_map>
#include <set>
#include <string>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include "OpenAL/AudioManager.hpp"
#include "Client-Server/ClientManager.hpp"
#include "netcode/client_window.hpp"
#include "Utils/Debug/Debug.hpp"

#include "Utils/AssetManager.hpp"
#include "Utils/Debug/SystemInfo.hpp"

#include <SOIL2/SOIL2.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include <tiny_gltf.h>

#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include "OpenGL/MeshBufferLoader.hpp"


class NetTFG_Engine {
public:
    static NetTFG_Engine& Get() {
        static NetTFG_Engine instance;
        return instance;
    }

    NetTFG_Engine(const NetTFG_Engine&) = delete;
    NetTFG_Engine& operator=(const NetTFG_Engine&) = delete;
    NetTFG_Engine(NetTFG_Engine&&) = delete;
    NetTFG_Engine& operator=(NetTFG_Engine&&) = delete;

    void RegisterClient(int id, Client* client) {
        auto& mgr = ClientManager::Get();
        size_t index = mgr.AddClient(std::unique_ptr<Client>(client));
        clientIndexMap[id] = index;
        Debug::Info("NetTFG_Engine") << "Registered client " << id << " at index " << index << "\n";
    }

    template<typename Callback>
    void ActivateClientAsync(int id, Callback&& callback,
        const std::string& host = "0.0.0.0",
        uint16_t port = 0,
        const std::string& customClientId = "") {
        std::thread([this, id, host, port, customClientId, cb = std::forward<Callback>(callback)]() mutable {
            Debug::SetThreadName("Connect");
            ConnectionCode code = ActivateClientInternal(id, host, port, customClientId) ? CONN_SUCCESS : CONN_TIMEOUT;

            auto it = clientIndexMap.find(id);
            if (code != CONN_SUCCESS && it != clientIndexMap.end()) {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto errorIt = clientErrorCodes_.find(it->second);
                if (errorIt != clientErrorCodes_.end()) {
                    code = errorIt->second;
                }
            }

            cb(id, code);
            }).detach();
    }

    // Synchronous version for backward compatibility
    bool ActivateClient(int id, const std::string& host = "0.0.0.0",
        uint16_t port = 0, const std::string& customClientId = "") {
        return ActivateClientInternal(id, host, port, customClientId);
    }

    bool DeactivateClient(int id) {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) {
            Debug::Warning("NetTFG_Engine") << "Cannot deactivate client " << id << ": not registered\n";
            return false;
        }

        size_t index = it->second;
        Client* client = ClientManager::Get().GetClient(index);

        if (client) {
            // AUDIO CLEANUP must happen before CloseClient()/unloadBin(): FlushEntities needs the RENDERER's EntityManager (where AudioSourceComponent/AudioListenerComponent live), not the logic one, so the audio thread detaches from this world before it's torn down.
            AudioManager::FlushEntities(client->GetRendererEntityManager());
            AudioManager::StopAllSources();

            // CloseClient() releases every mesh/texture/shader the client's ECS holds (on the render thread), dropping AssetManager ref-counts to 0; must run before unloadBin() or it sees refCount > 0 and leaks the bin.
            client->CloseClient();

            // unloadBin() runs each freed asset's GL destroyer inline, so it must happen on the render thread, same as CloseClient()'s ReleaseECSAssets().
            // glFinish around it like IECSGameRenderer::ReleaseECSAssets(): nothing in flight may use what gets deleted.
            ClientWindow::RunOnRenderThread([client]() {
                glFinish();
                AssetManager::instance().unloadBin(client->binName);
                glFinish();
                });
        }

        ClientManager::Get().DeactivateClient(index);

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            clientConnections.erase(index);
        }

        Debug::Info("NetTFG_Engine") << "Deactivated client " << id << " (index " << index << ")\n";
        return true;
    }

    // Deferred deactivation: queued and applied by the main loop between ticks, safe to call from inside a TickClient.
    void RequestDeactivateClient(int id) {
        std::lock_guard<std::mutex> lock(pendingDeactivationsMutex_);
        pendingDeactivations_.push_back(id);
    }

    // Deferred activation, queued like RequestDeactivateClient: use instead of synchronous ActivateClient() when called from the render thread (e.g. a UIButton::onClick), so SetupClient()'s stale-ClientWindow delete only ever runs during the render thread's own task-drain, never reentrantly (past cause of a menu->settings->menu use-after-free).
    void RequestActivateClient(int id, const std::string& host = "0.0.0.0",
        uint16_t port = 0, const std::string& customClientId = "") {
        std::lock_guard<std::mutex> lock(pendingActivationsMutex_);
        pendingActivations_.push_back({ id, host, port, customClientId });
    }

    void DeactivateAllClients() {
        auto& mgr = ClientManager::Get();
        auto activeIndices = mgr.GetActiveIndices();

        for (size_t index : activeIndices) {
            Client* client = mgr.GetClient(index);
            if (client) {
                // See DeactivateClient() above: must be the renderer's EM.
                AudioManager::FlushEntities(client->GetRendererEntityManager());
                AudioManager::StopAllSources();
                client->CloseClient();
            }
        }

        mgr.DeactivateAll();
        clientConnections.clear();
        Debug::Info("NetTFG_Engine") << "Deactivated all clients\n";
    }

    void Start(int width, int height, std::string windowName) {
        running_.store(true);
        ClientStartup(width, height, windowName);

        Debug::Info("NetTFG_Engine") << "Starting engine\n";

        const int MAX_CATCHUP_TICKS = 3;
        auto nextTick = std::chrono::steady_clock::now();

        // IsCloseRequested() is deliberately distinct from isWindowThreadRunning(): the render thread stays alive until ClientCleanup() below has released every client's GL resources and calls stopRenderThread() itself.
        while (running_.load() && ClientWindow::isWindowThreadRunning() && !ClientWindow::IsCloseRequested()) {
            auto& mgr = ClientManager::Get();
            auto activeIndices = mgr.GetActiveIndices();

            if (activeIndices.empty()) {
                Debug::Warning("NetTFG_Engine") << "No active clients, stopping engine\n";
                Stop();
                break;
            }

            for (size_t index : activeIndices) {
                Client* client = mgr.GetClient(index);
                if (client) {
                    try {
                        client->TickClient();
                    }
                    catch (const std::exception& e) {
                        Debug::Error("NetTFG_Engine") << "Exception in client tick: " << e.what() << "\n";
                        // Optionally deactivate the problematic client
                    }
                }
            }

            // Flush deferred activations, then deactivations, after all ticks return (no client is mid-tick); activations first so a scene transition never has zero clients active.
            {
                std::vector<PendingActivation> toActivate;
                {
                    std::lock_guard<std::mutex> lock(pendingActivationsMutex_);
                    toActivate.swap(pendingActivations_);
                }
                for (auto& pa : toActivate) {
                    ActivateClient(pa.id, pa.host, pa.port, pa.customClientId);
                }
            }
            {
                std::vector<int> toDeactivate;
                {
                    std::lock_guard<std::mutex> lock(pendingDeactivationsMutex_);
                    toDeactivate.swap(pendingDeactivations_);
                }
                for (int id : toDeactivate) {
                    DeactivateClient(id);
                }
            }

            // Fixed timestep - wait until next tick
            nextTick += TICK_DURATION;

            // Resync instead of bursting after a long stall (e.g. DeactivateClient blocked in RunOnRenderThread while the new
            // scene's Init runs): the burst ran an OnlineClient's prediction far ahead of the server, the next reconciliation
            // rewound it, and the local player froze until the frame caught up again, then snapped.
            const auto now = std::chrono::steady_clock::now();
            if (now - nextTick > TICK_DURATION * MAX_CATCHUP_TICKS) {
                nextTick = now;
            }
            std::this_thread::sleep_until(nextTick);
        }

        ClientCleanup();
        Debug::Info("NetTFG_Engine") << "Engine ended\n";
    }

    void Stop() {
        if (running_.load()) {
            running_.store(false);
            Debug::Info("NetTFG_Engine") << "Engine stop requested\n";
        }
    }

    bool IsRunning() const { return running_.load(); }

    size_t GetActiveClientCount() const {
        return ClientManager::Get().ActiveClientCount();
    }

    bool IsClientActive(int id) const {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return false;
        return ClientManager::Get().IsClientActive(it->second);
    }

    // True while an activation of this client is running (e.g. an OnlineClient waiting for its match to start).
    bool IsClientConnecting(int id) const {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return false;
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        return settingUp_.count(it->second) != 0;
    }

    bool IsClientActive(const Client* client) const {
        if (!client) return false;

        auto& mgr = ClientManager::Get();
        auto activeClients = mgr.GetActiveClients();

        return std::find(activeClients.begin(), activeClients.end(), client) != activeClients.end();
    }

    Client* GetClient(int id) {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return nullptr;
        return ClientManager::Get().GetClient(it->second);
    }

    const Client* GetClient(int id) const {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return nullptr;
        return ClientManager::Get().GetClient(it->second);
    }

    bool GetClientConnection(int id, std::string& host, uint16_t& port, std::string& name) const {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return false;

        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto connIt = clientConnections.find(it->second);
        if (connIt == clientConnections.end()) return false;

        host = connIt->second.host;
        port = connIt->second.port;
        name = connIt->second.name;
        return true;
    }

    template<typename Callback>
    void ReconnectClientAsync(int id, Callback&& callback) {
        std::thread([this, id, cb = std::forward<Callback>(callback)]() mutable {
            Debug::SetThreadName("Reconnect");
            ReconnectClientInternal(id);

            ConnectionCode code = CONN_TIMEOUT;
            auto it = clientIndexMap.find(id);
            if (it != clientIndexMap.end()) {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto errorIt = clientErrorCodes_.find(it->second);
                if (errorIt != clientErrorCodes_.end()) {
                    code = errorIt->second;
                }
                else {
                    code = CONN_TIMEOUT;
                }
            }

            cb(id, code);
            }).detach();
    }

    bool ReconnectClient(int id) {
        return ReconnectClientInternal(id);
    }

private:
    NetTFG_Engine()
    {
        // Two phases (see MeshBufferLoader.hpp): AssetManager::prefetch runs the CPU half on a worker thread while the
        // player waits for the match, and loadAsset on the render thread then only uploads.
        AssetManager::instance().registerTwoPhaseType<MeshBuffer, PreparedMesh>(
            [](const uint8_t* data, size_t size) { return MeshBufferLoader::PrepareMeshBuffer(data, size); },
            [](PreparedMesh& prepared) { return MeshBufferLoader::FinishMeshBuffer(prepared); },

            // Destroyer
            [](MeshBuffer buffer)
            {
                for (auto& sm : buffer.subMeshes) {
                    if (sm.diffuseTex)   glDeleteTextures(1, &sm.diffuseTex);
                    if (sm.normalTex)    glDeleteTextures(1, &sm.normalTex);
                    if (sm.mrTex)        glDeleteTextures(1, &sm.mrTex);
                    if (sm.occlusionTex) glDeleteTextures(1, &sm.occlusionTex);
                    if (sm.emissiveTex)  glDeleteTextures(1, &sm.emissiveTex);
                }
                if (buffer.EBO) glDeleteBuffers(1, &buffer.EBO);
                if (buffer.VBO) glDeleteBuffers(1, &buffer.VBO);
                if (buffer.VAO) glDeleteVertexArrays(1, &buffer.VAO);
            });

        AssetManager::instance().registerType<ShaderSource>(
            // Loader
            [](const uint8_t* data, size_t size) -> ShaderSource
            {
                return { std::string(reinterpret_cast<const char*>(data), size) };
            },
            // Destroyer
            [](ShaderSource) {}
        );

        AssetManager::instance().registerType<BlobData>(
            // Loader: keeps a copy of the decompressed bytes (the loader's buffer is scratch)
            [](const uint8_t* data, size_t size) -> BlobData
            {
                return { std::make_shared<const std::vector<uint8_t>>(data, data + size) };
            },
            // Destroyer: the shared_ptr frees the bytes
            [](BlobData) {}
        );

        AssetManager::instance().registerType<AudioBuffer>(
            // Loader
            [](const uint8_t* data, size_t size) -> AudioBuffer
            {
                AudioBuffer buffer;
                buffer.value = loadWavALFromMemory(data, size);
                return buffer;
            },

            // Destroyer
            [](AudioBuffer buffer)
            {
                if (buffer.value != 0)
                    alDeleteBuffers(1, &(buffer.value));
            }
        );

        AssetManager::instance().registerType<MusicBuffer>(
            // Loader: keeps stereo (music isn't positioned, so nothing gains from the mono downmix)
            [](const uint8_t* data, size_t size) -> MusicBuffer
            {
                MusicBuffer buffer;
                buffer.value = loadWavALFromMemory(data, size, true);
                return buffer;
            },

            // Destroyer
            [](MusicBuffer buffer)
            {
                if (buffer.value != 0)
                    alDeleteBuffers(1, &(buffer.value));
            }
        );

        AssetManager::instance().registerType<TextureID>(
            [](const uint8_t* data, size_t size) -> TextureID
            {

                TextureID texture;
                texture.value = 0;

                // No NTSC_SAFE_RGB (clamps to 16..235: black became 0.063, which HDR-tinted additive sprites turn into
                // visible squares). DXT stays: SOIL pads to power-of-two sizes, so the 4000^2 explosion sheet is a
                // 4096^2 texture, ~85 MB with mips uncompressed vs ~21 MB as DXT5 — too much for a 2 GB GPU.
                GLuint textureID = SOIL_load_OGL_texture_from_memory(
                    data,
                    static_cast<int>(size),
                    SOIL_LOAD_AUTO,
                    SOIL_CREATE_NEW_ID,
                    SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_COMPRESS_TO_DXT
                );

                if (textureID == 0) {
                    Debug::Error("AssetManager")
                        << "SOIL2 failed to load texture from memory: "
                        << SOIL_last_result() << "\n";
                    return texture;
                }

                glBindTexture(GL_TEXTURE_2D, textureID);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                Debug::Info("AssetManager")
                    << "Successfully loaded texture from memory (ID: " << textureID << ")\n";

                texture.value = textureID;

                return texture;
            },
            [](TextureID texture)
            {
                if (texture.value != 0)
                    glDeleteTextures(1, &(texture.value));
            }
        );


    }

    bool ReconnectClientInternal(int id) {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) {
            Debug::Warning("NetTFG_Engine") << "Cannot reconnect client " << id << ": not registered\n";
            return false;
        }

        size_t index = it->second;

        ConnectionInfo connInfo;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            auto connIt = clientConnections.find(index);
            if (connIt == clientConnections.end()) {
                Debug::Warning("NetTFG_Engine") << "No connection info for client " << id << "\n";
                return false;
            }
            connInfo = connIt->second;
        }

        if (IsClientActive(id)) {
            DeactivateClient(id);
        }

        return ActivateClientInternal(id, connInfo.host, connInfo.port, connInfo.name);
    }

    // Internal activation logic (thread-safe)
    bool ActivateClientInternal(int id, const std::string& host, uint16_t port, const std::string& customClientId) {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) {
            Debug::Warning("NetTFG_Engine") << "Cannot activate client " << id << ": not registered\n";
            return false;
        }

        size_t index = it->second;
        Client* client = ClientManager::Get().GetClient(index);

        if (!client) {
            Debug::Error("NetTFG_Engine") << "Client " << id << " is null\n";
            return false;
        }

        // One setup at a time per client, and never on an active one: SetupClient on a live client destroys the window
        // and prediction its network thread and tick still use (alpha test 2026-11-11: two Connect threads on the same
        // OnlineClient, and a client left active behind the settings menu).
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            if (settingUp_.count(index) || ClientManager::Get().IsClientActive(index)) {
                Debug::Warning("NetTFG_Engine") << "Client " << id << " is already "
                    << (settingUp_.count(index) ? "connecting" : "active") << ", activation ignored\n";
                clientErrorCodes_[index] = CONN_BUSY;
                return false;
            }
            settingUp_.insert(index);
        }
        struct SetupGuard {
            NetTFG_Engine* engine; size_t index;
            ~SetupGuard() { std::lock_guard<std::mutex> lock(engine->connectionsMutex_); engine->settingUp_.erase(index); }
        } setupGuard{ this, index };

        Debug::Info("NetTFG_Engine") << "Setting up client " << id << "...\n";

        if (client->keepAssetsResident)
            AssetManager::instance().setKeepResident(client->binName, true);
        AssetManager::instance().loadBin(client->binName);
        StartPrefetch(*client);

        // Setup the client with connection parameters (may block)
        ConnectionCode result = client->SetupClient(host, port, customClientId);

        if (result != CONN_SUCCESS) {
            Debug::Error("NetTFG_Engine") << "Failed to setup client " << id
                << " with error code: " << result << "\n";

            {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                clientErrorCodes_[index] = result;
            }
            return false;
        }

        ClientManager::Get().ActivateClient(index);

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            clientConnections[index] = { host, port, customClientId };
            clientErrorCodes_[index] = CONN_SUCCESS;
        }

        Debug::Info("NetTFG_Engine") << "Activated client " << id << " (index " << index << ")\n";
        return true;
    }

    // Prefetch needs room for the decoded images until the render thread uploads them (a few hundred MB for the big
    // meshes); below this much free RAM they load the old way, one at a time on the render thread.
    static constexpr uint64_t PREFETCH_MIN_FREE_RAM = 3ULL << 30;

    void StartPrefetch(const Client& client) {
        if (client.prefetchMeshes.empty()) return;

        const uint64_t freeRam = SystemInfo::AvailableMemoryBytes();
        if (freeRam != 0 && freeRam < PREFETCH_MIN_FREE_RAM) {
            Debug::Info("NetTFG_Engine") << "Skipping mesh prefetch: only " << (freeRam >> 20) << " MB of RAM free\n";
            return;
        }

        const size_t queued = AssetManager::instance().prefetch<MeshBuffer>(client.prefetchMeshes);
        if (queued > 0)
            Debug::Info("NetTFG_Engine") << "Prefetching " << queued << " meshes for " << client.binName << "\n";
    }

    void ClientStartup(int width, int height, std::string windowName) {
        AudioManager::Start();
        ClientWindow::startRenderThread(width, height, windowName);
        Debug::Info("NetTFG_Engine") << "Client systems started\n";
    }

    void ClientCleanup() {
        DeactivateAllClients();

        AudioManager::Stop();
        ClientWindow::stopRenderThread();
        Debug::Info("NetTFG_Engine") << "Client systems stopped\n";
    }

private:
    struct ConnectionInfo {
        std::string host;
        uint16_t port;
        std::string name;
    };

    struct PendingActivation {
        int id;
        std::string host;
        uint16_t port;
        std::string customClientId;
    };

    std::atomic<bool> running_{ false };
    std::unordered_map<int, size_t> clientIndexMap;  // Maps user-friendly ID to ClientManager index
    std::unordered_map<size_t, ConnectionInfo> clientConnections;  // Connection info per client index
    std::unordered_map<size_t, ConnectionCode> clientErrorCodes_;  // Last error code per client index
    mutable std::mutex connectionsMutex_;  // Protect clientConnections, clientErrorCodes_ and settingUp_ for async access
    std::set<size_t> settingUp_;           // Client indices inside ActivateClientInternal right now (see there)

    // Deferred deactivation queue: ActivateClientAsync callbacks fire on a background thread and can't call DeactivateClient directly (target may be mid-tick), so they queue here for Start() to drain post-tick.
    std::vector<int> pendingDeactivations_;
    std::mutex pendingDeactivationsMutex_;

    // Deferred activation queue; see RequestActivateClient(). Drained before pendingDeactivations_ by the same post-tick block in Start().
    std::vector<PendingActivation> pendingActivations_;
    std::mutex pendingActivationsMutex_;
};