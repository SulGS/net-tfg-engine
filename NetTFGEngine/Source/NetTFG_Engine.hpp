#pragma once
#include <unordered_map>
#include <string>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include "OpenAL/AudioManager.hpp"
#include "Client-Server/ClientManager.hpp"
#include "Utils/Debug/Debug.hpp"

#include "Utils/AssetManager.hpp"

#include <SOIL2/SOIL2.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include <tiny_gltf.h>


class NetTFG_Engine {
public:
    // ---- Singleton ----
    static NetTFG_Engine& Get() {
        static NetTFG_Engine instance;
        return instance;
    }

    // Prevent copy/move
    NetTFG_Engine(const NetTFG_Engine&) = delete;
    NetTFG_Engine& operator=(const NetTFG_Engine&) = delete;
    NetTFG_Engine(NetTFG_Engine&&) = delete;
    NetTFG_Engine& operator=(NetTFG_Engine&&) = delete;

    // ---- Register client using pointer ----
    void RegisterClient(int id, Client* client) {
        auto& mgr = ClientManager::Get();
        size_t index = mgr.AddClient(std::unique_ptr<Client>(client));
        clientIndexMap[id] = index;
        Debug::Info("NetTFG_Engine") << "Registered client " << id << " at index " << index << "\n";
    }

    // ---- Activate/Deactivate Clients ----

    // Async activation with callback
    template<typename Callback>
    void ActivateClientAsync(int id, Callback&& callback,
        const std::string& host = "0.0.0.0",
        uint16_t port = 0,
        const std::string& customClientId = "") {
        std::thread([this, id, host, port, customClientId, cb = std::forward<Callback>(callback)]() mutable {
            ActivateClientInternal(id, host, port, customClientId);

            // Retrieve the connection code
            ConnectionCode code = CONN_TIMEOUT;
            auto it = clientIndexMap.find(id);
            if (it != clientIndexMap.end()) {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto errorIt = clientErrorCodes_.find(it->second);
                if (errorIt != clientErrorCodes_.end()) {
                    code = errorIt->second;
                }
            }

            // Invoke callback with result
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

		AssetManager::instance().unloadBin(client->binName);

        if (client) {
            // Close the client properly
            client->CloseClient();
        }

        // Remove from active clients
        ClientManager::Get().DeactivateClient(index);

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            clientConnections.erase(index);
        }

        Debug::Info("NetTFG_Engine") << "Deactivated client " << id << " (index " << index << ")\n";
        return true;
    }

    void DeactivateAllClients() {
        auto& mgr = ClientManager::Get();
        auto activeIndices = mgr.GetActiveIndices();

        // Close all active clients
        for (size_t index : activeIndices) {
            Client* client = mgr.GetClient(index);
            if (client) {
                client->CloseClient();
            }
        }

        mgr.DeactivateAll();
        clientConnections.clear();
        Debug::Info("NetTFG_Engine") << "Deactivated all clients\n";
    }

    // ---- Main Engine Loop ----
    void Start(int width, int height, std::string windowName) {
        running_.store(true);
        ClientStartup(width, height, windowName);

        Debug::Info("NetTFG_Engine") << "Starting engine\n";

        const auto TICK_DURATION = std::chrono::microseconds(1000000 / TICKS_PER_SECOND);
        auto nextTick = std::chrono::steady_clock::now();

        while (running_.load() && ClientWindow::isWindowThreadRunning()) {
            auto& mgr = ClientManager::Get();
            auto activeIndices = mgr.GetActiveIndices();

            if (activeIndices.empty()) {
                Debug::Warning("NetTFG_Engine") << "No active clients, stopping engine\n";
                Stop();
                break;
            }

            // Tick all active clients
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

            // Fixed timestep - wait until next tick
            nextTick += TICK_DURATION;
            std::this_thread::sleep_until(nextTick);
        }

        ClientCleanup();
        Debug::Info("NetTFG_Engine") << "Engine ended\n";
    }

    // ---- Control ----
    void Stop() {
        if (running_.load()) {
            running_.store(false);
            Debug::Info("NetTFG_Engine") << "Engine stop requested\n";
        }
    }

    bool IsRunning() const { return running_.load(); }

    // ---- Query Active Clients ----
    size_t GetActiveClientCount() const {
        return ClientManager::Get().ActiveClientCount();
    }

    bool IsClientActive(int id) const {
        auto it = clientIndexMap.find(id);
        if (it == clientIndexMap.end()) return false;
        return ClientManager::Get().IsClientActive(it->second);
    }

    bool IsClientActive(const Client* client) const {
        if (!client) return false;

        auto& mgr = ClientManager::Get();
        auto activeClients = mgr.GetActiveClients();

        return std::find(activeClients.begin(), activeClients.end(), client) != activeClients.end();
    }

    // ---- Get Client Instances ----
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

    // ---- Get Client Connection Info ----
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

    // ---- Reconnect Client ----
    template<typename Callback>
    void ReconnectClientAsync(int id, Callback&& callback) {
        std::thread([this, id, cb = std::forward<Callback>(callback)]() mutable {
            ReconnectClientInternal(id);

            // Retrieve the connection code
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
        AssetManager::instance().registerType<MeshBuffer>(
            // Loader
            [](const uint8_t* data, size_t size) -> MeshBuffer
            {
                MeshBuffer buffer{};
                tinygltf::TinyGLTF loader;
                tinygltf::Model model;
                std::string err, warn;

                if (!loader.LoadBinaryFromMemory(&model, &err, &warn, data, size, "")) {
                    Debug::Error("GLTF") << err << "\n";
                    return buffer;
                }

                if (!warn.empty())
                    Debug::Warning("GLTF") << warn << "\n";

                std::vector<Vertex> vertices;
                std::vector<uint32_t> indices;

                // ---- We'll pack all primitives into a single VBO/EBO ----
                for (const auto& mesh : model.meshes) {
                    for (const auto& prim : mesh.primitives) {
                        // --- Extract attributes ---
                        auto getAccessorFloat = [&](const std::string& name) -> const float* {
                            auto it = prim.attributes.find(name);
                            if (it == prim.attributes.end()) return nullptr;
                            const auto& acc = model.accessors[it->second];
                            const auto& view = model.bufferViews[acc.bufferView];
                            const auto& buf = model.buffers[view.buffer];
                            return reinterpret_cast<const float*>(buf.data.data() + view.byteOffset + acc.byteOffset);
                            };

                        const float* pos = getAccessorFloat("POSITION");
                        const float* norm = getAccessorFloat("NORMAL");
                        const float* uv = getAccessorFloat("TEXCOORD_0");

                        if (!pos) continue; // must have positions

                        size_t vertOffset = vertices.size();
                        const auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
                        vertices.resize(vertOffset + posAcc.count);

                        for (size_t i = 0; i < posAcc.count; ++i) {
                            Vertex& v = vertices[vertOffset + i];
                            v.position = glm::vec3(pos[i * 3 + 0], pos[i * 3 + 1], pos[i * 3 + 2]);
                            v.normal = norm ? glm::vec3(norm[i * 3 + 0], norm[i * 3 + 1], norm[i * 3 + 2])
                                : glm::vec3(0.0f, 1.0f, 0.0f);
                            v.uv = uv ? glm::vec2(uv[i * 2 + 0], uv[i * 2 + 1])
                                : glm::vec2(0.0f, 0.0f);
                        }

                        // --- Extract indices ---
                        std::vector<uint32_t> primIndices;
                        if (prim.indices >= 0) {
                            const auto& idxAcc = model.accessors[prim.indices];
                            const auto& idxView = model.bufferViews[idxAcc.bufferView];
                            const auto& idxBuf = model.buffers[idxView.buffer];

                            primIndices.resize(idxAcc.count);
                            for (size_t i = 0; i < idxAcc.count; ++i) {
                                if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                                    primIndices[i] = static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i]);
                                else
                                    primIndices[i] = reinterpret_cast<const uint32_t*>(idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i];
                                primIndices[i] += static_cast<uint32_t>(vertOffset); // adjust for global VBO
                            }

                            indices.insert(indices.end(), primIndices.begin(), primIndices.end());
                        }

                        // --- Load texture for this primitive ---
                        GLuint texID = 0;
                        if (prim.material >= 0 && prim.material < model.materials.size()) {
                            const auto& mat = model.materials[prim.material];
                            if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                                int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
                                if (texIdx >= 0 && texIdx < model.textures.size()) {
                                    const auto& tex = model.textures[texIdx];
                                    const auto& img = model.images[tex.source];

                                    glGenTextures(1, &texID);
                                    glBindTexture(GL_TEXTURE_2D, texID);
                                    GLint fmt = img.component == 3 ? GL_RGB : GL_RGBA;
                                    glTexImage2D(GL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(img.width),
                                        static_cast<GLsizei>(img.height), 0, fmt, GL_UNSIGNED_BYTE, img.image.data());
                                    glGenerateMipmap(GL_TEXTURE_2D);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                }
                            }
                        }

                        // --- Save submesh range ---
                        SubMeshRange smr;
                        smr.indexOffset = static_cast<uint32_t>(indices.size() - primIndices.size());
                        smr.indexCount = static_cast<uint32_t>(primIndices.size());
                        smr.diffuseTex = texID;
                        buffer.subMeshes.push_back(smr);
                    }
                }

                // --- Upload to GPU ---
                glGenVertexArrays(1, &buffer.VAO);
                glGenBuffers(1, &buffer.VBO);
                glGenBuffers(1, &buffer.EBO);

                glBindVertexArray(buffer.VAO);

                glBindBuffer(GL_ARRAY_BUFFER, buffer.VBO);
                glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.EBO);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
                glEnableVertexAttribArray(2);

                glBindVertexArray(0);

                return buffer;
            },
            // Destroyer
            [](MeshBuffer buffer) {
                for (auto& sm : buffer.subMeshes)
                    if (sm.diffuseTex) glDeleteTextures(1, &sm.diffuseTex);
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

        AssetManager::instance().registerType<TextureID>(
            [](const uint8_t* data, size_t size) -> TextureID
            {

                TextureID texture;
				texture.value = 0;

                GLuint textureID = SOIL_load_OGL_texture_from_memory(
                    data,
                    static_cast<int>(size),
                    SOIL_LOAD_AUTO,
                    SOIL_CREATE_NEW_ID,
                    SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT
                );

                if (textureID == 0) {
                    Debug::Error("AssetManager")
                        << "SOIL2 failed to load texture from memory: "
                        << SOIL_last_result() << "\n";
                    return texture;
                }

                // Set texture parameters
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

        // Deactivate first
        if (IsClientActive(id)) {
            DeactivateClient(id);
        }

        // Reactivate with stored connection info
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

        Debug::Info("NetTFG_Engine") << "Setting up client " << id << "...\n";

		AssetManager::instance().loadBin(client->binName);

        // Setup the client with connection parameters (may block)
        ConnectionCode result = client->SetupClient(host, port, customClientId);

        if (result != CONN_SUCCESS) {
            Debug::Error("NetTFG_Engine") << "Failed to setup client " << id
                << " with error code: " << result << "\n";

            // Store the error code for callback access
            {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                clientErrorCodes_[index] = result;
            }
            return false;
        }

        // Add to active clients in ClientManager
        ClientManager::Get().ActivateClient(index);

        // Store connection info for this client (thread-safe with mutex)
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            clientConnections[index] = { host, port, customClientId };
            clientErrorCodes_[index] = CONN_SUCCESS;
        }

        Debug::Info("NetTFG_Engine") << "Activated client " << id << " (index " << index << ")\n";
        return true;
    }

    void ClientStartup(int width, int height, std::string windowName) {
        AudioManager::Start();
        ClientWindow::startRenderThread(width, height, windowName);
        Debug::Info("NetTFG_Engine") << "Client systems started\n";
    }

    void ClientCleanup() {
        // Properly close all clients before shutting down
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

    std::atomic<bool> running_{ false };
    std::unordered_map<int, size_t> clientIndexMap;  // Maps user-friendly ID to ClientManager index
    std::unordered_map<size_t, ConnectionInfo> clientConnections;  // Connection info per client index
    std::unordered_map<size_t, ConnectionCode> clientErrorCodes_;  // Last error code per client index
    mutable std::mutex connectionsMutex_;  // Protect clientConnections and clientErrorCodes_ for async access
};