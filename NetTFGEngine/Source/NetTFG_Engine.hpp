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
#include "netcode/client_window.hpp"
#include "Utils/Debug/Debug.hpp"

#include "Utils/AssetManager.hpp"

#include <SOIL2/SOIL2.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include <tiny_gltf.h>

#include "OpenGL/Render pipeline/RenderSettings.hpp"


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
            ActivateClientInternal(id, host, port, customClientId);

            ConnectionCode code = CONN_TIMEOUT;
            auto it = clientIndexMap.find(id);
            if (it != clientIndexMap.end()) {
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
            ClientWindow::RunOnRenderThread([client]() {
                AssetManager::instance().unloadBin(client->binName);
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

        const auto TICK_DURATION = std::chrono::microseconds(1000000 / TICKS_PER_SECOND);
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
        AssetManager::instance().registerType<MeshBuffer>(
            // Loader
            [](const uint8_t* data, size_t size) -> MeshBuffer
            {
                MeshBuffer buffer{};
                tinygltf::TinyGLTF loader;
                tinygltf::Model    model;
                std::string        err, warn;

                if (!loader.LoadBinaryFromMemory(&model, &err, &warn, data, size, "")) {
                    Debug::Error("GLTF") << err << "\n";
                    return buffer;
                }

                if (!warn.empty())
                    Debug::Warning("GLTF") << warn << "\n";

                std::vector<Vertex>   vertices;
                std::vector<uint32_t> indices;

                // Read quality settings once for the whole load
                const auto& rs = RenderSettings::instance();
                const int    baseMip = rs.texBaseMip();
                const bool   useCompression = rs.texCompression();

                // Texture cache keyed by (imageSource << 1 | sRGB) so the same image uploaded sRGB vs linear gets two separate GL textures.
                std::unordered_map<uint64_t, GLuint> texCache;

                // isNormalMap applies GL_TEXTURE_LOD_BIAS=2.0: at large world scale the GPU otherwise always picks mip 0, producing a distinct specular dot on normal maps.
                auto loadTex = [&](int texIndex, bool sRGB, bool isNormalMap = false) -> GLuint
                    {
                        if (texIndex < 0 || texIndex >= (int)model.textures.size())
                            return 0;

                        const auto& tex = model.textures[texIndex];
                        if (tex.source < 0 || tex.source >= (int)model.images.size())
                            return 0;

                        uint64_t key = ((uint64_t)tex.source << 1) | (sRGB ? 1u : 0u);
                        auto it = texCache.find(key);
                        if (it != texCache.end())
                            return it->second;

                        const auto& img = model.images[tex.source];

                        GLint internalFmt, uploadFmt;
                        if (useCompression) {
                            // BC7/BPTC — driver compresses at upload time
                            internalFmt = sRGB ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                                : GL_COMPRESSED_RGBA_BPTC_UNORM;
                            uploadFmt = GL_RGBA; // BC7 requires RGBA input
                        }
                        else {
                            if (img.component == 4) {
                                internalFmt = sRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
                                uploadFmt = GL_RGBA;
                            }
                            else {
                                internalFmt = sRGB ? GL_SRGB8 : GL_RGB8;
                                uploadFmt = GL_RGB;
                            }
                        }

                        // If compression is on or the source is RGB, pad to RGBA (BC7 always needs 4 channels).
                        std::vector<uint8_t> paddedRGBA;
                        const uint8_t* uploadData = img.image.data();

                        if ((useCompression || uploadFmt == GL_RGBA) && img.component == 3)
                        {
                            paddedRGBA.resize(img.width * img.height * 4);
                            for (int i = 0; i < img.width * img.height; ++i)
                            {
                                paddedRGBA[i * 4 + 0] = img.image[i * 3 + 0];
                                paddedRGBA[i * 4 + 1] = img.image[i * 3 + 1];
                                paddedRGBA[i * 4 + 2] = img.image[i * 3 + 2];
                                paddedRGBA[i * 4 + 3] = 255;
                            }
                            uploadData = paddedRGBA.data();
                        }

                        // Resolution reduction: box-filter downsample on CPU by skipping `baseMip` mip levels.
                        std::vector<uint8_t> downsampled;
                        int uploadW = img.width;
                        int uploadH = img.height;

                        if (baseMip > 0)
                        {
                            uploadW = std::max(1, img.width >> baseMip);
                            uploadH = std::max(1, img.height >> baseMip);

                            int scale = 1 << baseMip;
                            int components = 4; // always RGBA at this point
                            int sampleArea = scale * scale;
                            downsampled.resize(uploadW * uploadH * components);

                            auto toLinear = [](uint8_t v) -> float {
                                float f = v / 255.0f;
                                return f * f;
                                };
                            auto toSRGB = [](float v) -> uint8_t {
                                v = glm::clamp(v, 0.0f, 1.0f);
                                return static_cast<uint8_t>(sqrtf(v) * 255.0f + 0.5f);
                                };

                            for (int y = 0; y < uploadH; ++y)
                            {
                                for (int x = 0; x < uploadW; ++x)
                                {
                                    for (int c = 0; c < components; ++c)
                                    {
                                        bool isAlpha = (c == 3);
                                        bool doGamma = sRGB && !isAlpha;

                                        if (doGamma)
                                        {
                                            float sum = 0.0f;
                                            for (int dy = 0; dy < scale; ++dy)
                                                for (int dx = 0; dx < scale; ++dx)
                                                {
                                                    int sx = std::min(x * scale + dx, img.width - 1);
                                                    int sy = std::min(y * scale + dy, img.height - 1);
                                                    sum += toLinear(uploadData[(sy * img.width + sx) * components + c]);
                                                }
                                            downsampled[(y * uploadW + x) * components + c] =
                                                toSRGB(sum / sampleArea);
                                        }
                                        else
                                        {
                                            uint32_t sum = 0;
                                            for (int dy = 0; dy < scale; ++dy)
                                                for (int dx = 0; dx < scale; ++dx)
                                                {
                                                    int sx = std::min(x * scale + dx, img.width - 1);
                                                    int sy = std::min(y * scale + dy, img.height - 1);
                                                    sum += uploadData[(sy * img.width + sx) * components + c];
                                                }
                                            downsampled[(y * uploadW + x) * components + c] =
                                                static_cast<uint8_t>(sum / sampleArea);
                                        }
                                    }
                                }
                            }

                            uploadData = downsampled.data();
                        }

                        // Upload to GPU
                        GLuint id = 0;
                        glGenTextures(1, &id);
                        glBindTexture(GL_TEXTURE_2D, id);
                        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt,
                            (GLsizei)uploadW, (GLsizei)uploadH,
                            0, uploadFmt, GL_UNSIGNED_BYTE, uploadData);
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                            RenderSettings::instance().getAnisotropy());

                        if (isNormalMap)
                            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.0f);

                        glBindTexture(GL_TEXTURE_2D, 0);

                        texCache[key] = id;
                        return id;
                    };

                // Stride-aware accessor helper.
                auto getAccessor = [&](const tinygltf::Primitive& prim,
                    const std::string& name)
                    -> std::function<const float* (size_t)>
                    {
                        auto it = prim.attributes.find(name);
                        if (it == prim.attributes.end())
                            return nullptr;

                        const auto& acc = model.accessors[it->second];
                        const auto& view = model.bufferViews[acc.bufferView];
                        const auto& buf = model.buffers[view.buffer];
                        const uint8_t* base = buf.data.data()
                            + view.byteOffset
                            + acc.byteOffset;

                        size_t componentSize = tinygltf::GetComponentSizeInBytes(acc.componentType);
                        size_t numComponents = tinygltf::GetNumComponentsInType(acc.type);
                        size_t stride = (view.byteStride != 0)
                            ? view.byteStride
                            : componentSize * numComponents;

                        return [base, stride](size_t i) -> const float* {
                            return reinterpret_cast<const float*>(base + i * stride);
                            };
                    };

                // Pack all primitives into a single VBO/EBO
                for (const auto& mesh : model.meshes) {
                    for (const auto& prim : mesh.primitives) {

                        auto getPos = getAccessor(prim, "POSITION");
                        auto getNorm = getAccessor(prim, "NORMAL");
                        auto getUV = getAccessor(prim, "TEXCOORD_0");
                        auto getTan = getAccessor(prim, "TANGENT");

                        if (!getPos) continue;

                        const auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
                        size_t      vertOffset = vertices.size();
                        vertices.resize(vertOffset + posAcc.count);

                        for (size_t i = 0; i < posAcc.count; ++i) {
                            Vertex& v = vertices[vertOffset + i];

                            const float* p = getPos(i);
                            v.position = glm::vec3(p[0], p[1], p[2]);

                            if (getNorm) {
                                const float* n = getNorm(i);
                                v.normal = glm::vec3(n[0], n[1], n[2]);
                            }
                            else {
                                v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                            }

                            if (getUV) {
                                const float* u = getUV(i);
                                v.uv = glm::vec2(u[0], u[1]);
                            }
                            else {
                                v.uv = glm::vec2(0.0f);
                            }

                            if (getTan) {
                                const float* t = getTan(i);
                                v.tangent = glm::vec4(t[0], t[1], t[2], t[3]);
                            }
                        }

                        // Generate tangents if the mesh didn't provide them
                        if (!getTan) {
                            std::vector<glm::vec3> tangentAccum(posAcc.count, glm::vec3(0.0f));

                            if (prim.indices >= 0) {
                                const auto& idxAcc = model.accessors[prim.indices];
                                const auto& idxView = model.bufferViews[idxAcc.bufferView];
                                const auto& idxBuf = model.buffers[idxView.buffer];

                                auto getIdx = [&](size_t i) -> uint32_t {
                                    if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                                        return reinterpret_cast<const uint16_t*>(
                                            idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i];
                                    return reinterpret_cast<const uint32_t*>(
                                        idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i];
                                    };

                                for (size_t i = 0; i + 2 < idxAcc.count; i += 3) {
                                    uint32_t i0 = getIdx(i),
                                        i1 = getIdx(i + 1),
                                        i2 = getIdx(i + 2);

                                    Vertex& v0 = vertices[vertOffset + i0];
                                    Vertex& v1 = vertices[vertOffset + i1];
                                    Vertex& v2 = vertices[vertOffset + i2];

                                    glm::vec3 edge1 = v1.position - v0.position;
                                    glm::vec3 edge2 = v2.position - v0.position;
                                    glm::vec2 dUV1 = v1.uv - v0.uv;
                                    glm::vec2 dUV2 = v2.uv - v0.uv;

                                    float det = dUV1.x * dUV2.y - dUV2.x * dUV1.y;
                                    if (glm::abs(det) < 1e-6f) continue;

                                    float     f = 1.0f / det;
                                    glm::vec3 T = f * (dUV2.y * edge1 - dUV1.y * edge2);

                                    tangentAccum[i0] += T;
                                    tangentAccum[i1] += T;
                                    tangentAccum[i2] += T;
                                }
                            }

                            for (size_t i = 0; i < posAcc.count; ++i) {
                                Vertex& v = vertices[vertOffset + i];
                                glm::vec3 N = v.normal;
                                glm::vec3 T = tangentAccum[i];

                                if (glm::length(T) < 1e-6f) {
                                    if (glm::abs(N.x) > 0.9f)
                                        T = glm::normalize(glm::cross(N, glm::vec3(0.0f, 1.0f, 0.0f)));
                                    else
                                        T = glm::normalize(glm::cross(N, glm::vec3(1.0f, 0.0f, 0.0f)));
                                }
                                else {
                                    T = glm::normalize(T - glm::dot(T, N) * N);
                                }

                                v.tangent = glm::vec4(T, 1.0f);
                            }
                        }

                        std::vector<uint32_t> primIndices;
                        if (prim.indices >= 0) {
                            const auto& idxAcc = model.accessors[prim.indices];
                            const auto& idxView = model.bufferViews[idxAcc.bufferView];
                            const auto& idxBuf = model.buffers[idxView.buffer];

                            primIndices.resize(idxAcc.count);
                            for (size_t i = 0; i < idxAcc.count; ++i) {
                                if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                                    primIndices[i] = static_cast<uint32_t>(
                                        reinterpret_cast<const uint16_t*>(
                                            idxBuf.data.data()
                                            + idxView.byteOffset
                                            + idxAcc.byteOffset)[i]);
                                else
                                    primIndices[i] =
                                    reinterpret_cast<const uint32_t*>(
                                        idxBuf.data.data()
                                        + idxView.byteOffset
                                        + idxAcc.byteOffset)[i];

                                primIndices[i] += static_cast<uint32_t>(vertOffset);
                            }
                            indices.insert(indices.end(), primIndices.begin(), primIndices.end());
                        }

                        // Load PBR textures; normalTex passes isNormalMap=true to apply LOD bias.
                        SubMeshRange smr{};
                        smr.indexOffset = static_cast<uint32_t>(indices.size() - primIndices.size());
                        smr.indexCount = static_cast<uint32_t>(primIndices.size());

                        if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                            const auto& mat = model.materials[prim.material];
                            const auto& pbr = mat.pbrMetallicRoughness;

                            smr.diffuseTex = loadTex(pbr.baseColorTexture.index, true);        // sRGB
                            smr.normalTex = loadTex(mat.normalTexture.index, false, true);  // linear, isNormalMap
                            smr.mrTex = loadTex(pbr.metallicRoughnessTexture.index, false);        // linear
                            smr.occlusionTex = loadTex(mat.occlusionTexture.index, false);        // linear
                            smr.emissiveTex = loadTex(mat.emissiveTexture.index, false);        // linear
                        }

                        buffer.subMeshes.push_back(smr);
                    }
                }

                glGenVertexArrays(1, &buffer.VAO);
                glGenBuffers(1, &buffer.VBO);
                glGenBuffers(1, &buffer.EBO);

                glBindVertexArray(buffer.VAO);

                glBindBuffer(GL_ARRAY_BUFFER, buffer.VBO);
                glBufferData(GL_ARRAY_BUFFER,
                    vertices.size() * sizeof(Vertex),
                    vertices.data(), GL_STATIC_DRAW);

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.EBO);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    indices.size() * sizeof(uint32_t),
                    indices.data(), GL_STATIC_DRAW);

                // location 0 — position
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                    (void*)offsetof(Vertex, position));
                glEnableVertexAttribArray(0);

                // location 1 — normal
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                    (void*)offsetof(Vertex, normal));
                glEnableVertexAttribArray(1);

                // location 2 — uv
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                    (void*)offsetof(Vertex, uv));
                glEnableVertexAttribArray(2);

                // location 3 — tangent (vec4, w = bitangent handedness)
                glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                    (void*)offsetof(Vertex, tangent));
                glEnableVertexAttribArray(3);

                glBindVertexArray(0);

                return buffer;
            },

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

        Debug::Info("NetTFG_Engine") << "Setting up client " << id << "...\n";

        AssetManager::instance().loadBin(client->binName);

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
    mutable std::mutex connectionsMutex_;  // Protect clientConnections and clientErrorCodes_ for async access

    // Deferred deactivation queue: ActivateClientAsync callbacks fire on a background thread and can't call DeactivateClient directly (target may be mid-tick), so they queue here for Start() to drain post-tick.
    std::vector<int> pendingDeactivations_;
    std::mutex pendingDeactivationsMutex_;

    // Deferred activation queue; see RequestActivateClient(). Drained before pendingDeactivations_ by the same post-tick block in Start().
    std::vector<PendingActivation> pendingActivations_;
    std::mutex pendingActivationsMutex_;
};