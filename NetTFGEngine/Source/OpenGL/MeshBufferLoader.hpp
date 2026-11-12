#pragma once

// glTF -> MeshBuffer in two phases, so the slow CPU half can run off the render thread (AssetManager::prefetch):
//   PrepareMeshBuffer: parse the .glb (tinygltf decodes the embedded PNG/JPG with stb_image), build the vertex/index
//                      arrays and tangents, and every texture's upload data (RGBA padding, the baseMip downsample and,
//                      for compressed textures, the CPU mip chain). Any thread, no GL.
//   FinishMeshBuffer:  create the GL buffers and textures from that. Render thread only.
// Included from NetTFG_Engine.hpp after tiny_gltf.h (which carries TINYGLTF_IMPLEMENTATION there).

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

#include "OpenGL/OpenGLIncludes.hpp"
#include "Utils/AssetManager.hpp"
#include "Utils/Debug/Debug.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"

struct PreparedTexture {
    int width = 0;
    int height = 0;
    GLint internalFormat = 0;
    GLenum uploadFormat = 0;
    bool isNormalMap = false;
    bool compressed = false;                    // true: every level is in `levels`; false: level 0 + glGenerateMipmap
    std::vector<std::vector<uint8_t>> levels;   // level 0 first
};

struct PreparedSubMesh {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    int diffuseTex = -1;    // indices into PreparedMesh::textures, -1 = none
    int normalTex = -1;
    int mrTex = -1;
    int occlusionTex = -1;
    int emissiveTex = -1;
};

struct PreparedMesh {
    bool ok = false;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<PreparedSubMesh> subMeshes;
    std::vector<PreparedTexture> textures;
    glm::vec3 boundsCenter{ 0.0f }; // see MeshBuffer::boundsRadius
    float     boundsRadius = -1.0f;
};

namespace MeshBufferLoader
{
    inline float ToLinear(uint8_t v) { const float f = v / 255.0f; return f * f; }
    inline uint8_t ToSRGB(float v) { return static_cast<uint8_t>(sqrtf(glm::clamp(v, 0.0f, 1.0f)) * 255.0f + 0.5f); }

    inline PreparedTexture PrepareTexture(const tinygltf::Image& img, bool sRGB, bool isNormalMap, int baseMip, bool useCompression)
    {
        PreparedTexture out;
        out.isNormalMap = isNormalMap;
        out.compressed = useCompression;

        if (useCompression) {
            // BC7/BPTC: the driver compresses at upload time
            out.internalFormat = sRGB ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
            out.uploadFormat = GL_RGBA; // BC7 requires RGBA input
        }
        else if (img.component == 4) {
            out.internalFormat = sRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            out.uploadFormat = GL_RGBA;
        }
        else {
            out.internalFormat = sRGB ? GL_SRGB8 : GL_RGB8;
            out.uploadFormat = GL_RGB;
        }

        // If compression is on or the source is RGB, pad to RGBA (BC7 always needs 4 channels).
        std::vector<uint8_t> paddedRGBA;
        const uint8_t* uploadData = img.image.data();
        if ((useCompression || out.uploadFormat == GL_RGBA) && img.component == 3) {
            paddedRGBA.resize(static_cast<size_t>(img.width) * img.height * 4);
            for (int i = 0; i < img.width * img.height; ++i) {
                paddedRGBA[i * 4 + 0] = img.image[i * 3 + 0];
                paddedRGBA[i * 4 + 1] = img.image[i * 3 + 1];
                paddedRGBA[i * 4 + 2] = img.image[i * 3 + 2];
                paddedRGBA[i * 4 + 3] = 255;
            }
            uploadData = paddedRGBA.data();
        }

        const int components = (out.uploadFormat == GL_RGBA) ? 4 : 3;
        int uploadW = img.width;
        int uploadH = img.height;

        // Resolution reduction: box-filter downsample on CPU by skipping `baseMip` mip levels.
        std::vector<uint8_t> level0;
        if (baseMip > 0) {
            uploadW = std::max(1, img.width >> baseMip);
            uploadH = std::max(1, img.height >> baseMip);
            const int scale = 1 << baseMip;
            const int sampleArea = scale * scale;
            level0.resize(static_cast<size_t>(uploadW) * uploadH * components);

            for (int y = 0; y < uploadH; ++y)
                for (int x = 0; x < uploadW; ++x)
                    for (int c = 0; c < components; ++c) {
                        const bool doGamma = sRGB && c != 3;
                        if (doGamma) {
                            float sum = 0.0f;
                            for (int dy = 0; dy < scale; ++dy)
                                for (int dx = 0; dx < scale; ++dx) {
                                    const int sx = std::min(x * scale + dx, img.width - 1);
                                    const int sy = std::min(y * scale + dy, img.height - 1);
                                    sum += ToLinear(uploadData[(static_cast<size_t>(sy) * img.width + sx) * components + c]);
                                }
                            level0[(static_cast<size_t>(y) * uploadW + x) * components + c] = ToSRGB(sum / sampleArea);
                        }
                        else {
                            uint32_t sum = 0;
                            for (int dy = 0; dy < scale; ++dy)
                                for (int dx = 0; dx < scale; ++dx) {
                                    const int sx = std::min(x * scale + dx, img.width - 1);
                                    const int sy = std::min(y * scale + dy, img.height - 1);
                                    sum += uploadData[(static_cast<size_t>(sy) * img.width + sx) * components + c];
                                }
                            level0[(static_cast<size_t>(y) * uploadW + x) * components + c] = static_cast<uint8_t>(sum / sampleArea);
                        }
                    }
        }
        else {
            level0.assign(uploadData, uploadData + static_cast<size_t>(uploadW) * uploadH * components);
        }

        out.width = uploadW;
        out.height = uploadH;
        out.levels.push_back(std::move(level0));

        if (useCompression) {
            // BC7 is not color-renderable, so glGenerateMipmap on it is INVALID_OPERATION per spec; drivers that accept
            // it decompress/recompress internally, and the Radeon 520's 21.19 driver crashed inside itself in the first
            // draw after a match's textures were freed. Build the chain on the CPU (2x2 box, gamma-aware for sRGB
            // colour) and let the driver compress each level instead.
            int w = uploadW, h = uploadH;
            while (w > 1 || h > 1) {
                const std::vector<uint8_t>& prev = out.levels.back();
                const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
                for (int y = 0; y < nh; ++y)
                    for (int x = 0; x < nw; ++x)
                        for (int c = 0; c < 4; ++c) {
                            const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                            const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                            const uint8_t s[4] = {
                                prev[(static_cast<size_t>(y0) * w + x0) * 4 + c], prev[(static_cast<size_t>(y0) * w + x1) * 4 + c],
                                prev[(static_cast<size_t>(y1) * w + x0) * 4 + c], prev[(static_cast<size_t>(y1) * w + x1) * 4 + c] };
                            uint8_t v;
                            if (sRGB && c != 3)
                                v = ToSRGB((ToLinear(s[0]) + ToLinear(s[1]) + ToLinear(s[2]) + ToLinear(s[3])) * 0.25f);
                            else
                                v = static_cast<uint8_t>((s[0] + s[1] + s[2] + s[3] + 2) / 4);
                            next[(static_cast<size_t>(y) * nw + x) * 4 + c] = v;
                        }
                out.levels.push_back(std::move(next));
                w = nw;
                h = nh;
            }
        }

        return out;
    }

    // CPU half. Settings (texture base mip, compression) are read here, at prepare time.
    inline PreparedMesh PrepareMeshBuffer(const uint8_t* data, size_t size)
    {
        PreparedMesh out;

        tinygltf::TinyGLTF loader;
        tinygltf::Model    model;
        std::string        err, warn;

        if (!loader.LoadBinaryFromMemory(&model, &err, &warn, data, static_cast<unsigned int>(size), "")) {
            Debug::Error("GLTF") << err << "\n";
            return out;
        }
        if (!warn.empty())
            Debug::Warning("GLTF") << warn << "\n";

        const auto& rs = RenderSettings::instance();
        const int  baseMip = rs.texBaseMip();
        const bool useCompression = rs.texCompression();

        // Keyed by (imageSource << 1 | sRGB) so the same image used sRGB and linear gets two separate GL textures.
        std::unordered_map<uint64_t, int> texIndexByKey;

        // isNormalMap: see FinishMeshBuffer (LOD bias).
        auto prepareTex = [&](int texIndex, bool sRGB, bool isNormalMap = false) -> int {
            if (texIndex < 0 || texIndex >= (int)model.textures.size())
                return -1;
            const auto& tex = model.textures[texIndex];
            if (tex.source < 0 || tex.source >= (int)model.images.size())
                return -1;

            const uint64_t key = ((uint64_t)tex.source << 1) | (sRGB ? 1u : 0u);
            auto it = texIndexByKey.find(key);
            if (it != texIndexByKey.end())
                return it->second;

            out.textures.push_back(PrepareTexture(model.images[tex.source], sRGB, isNormalMap, baseMip, useCompression));
            const int index = static_cast<int>(out.textures.size()) - 1;
            texIndexByKey[key] = index;
            return index;
        };

        // Stride-aware accessor helper.
        auto getAccessor = [&](const tinygltf::Primitive& prim, const std::string& name) -> std::function<const float* (size_t)> {
            auto it = prim.attributes.find(name);
            if (it == prim.attributes.end())
                return nullptr;

            const auto& acc = model.accessors[it->second];
            const auto& view = model.bufferViews[acc.bufferView];
            const auto& buf = model.buffers[view.buffer];
            const uint8_t* base = buf.data.data() + view.byteOffset + acc.byteOffset;

            const size_t componentSize = tinygltf::GetComponentSizeInBytes(acc.componentType);
            const size_t numComponents = tinygltf::GetNumComponentsInType(acc.type);
            const size_t stride = (view.byteStride != 0) ? view.byteStride : componentSize * numComponents;

            return [base, stride](size_t i) -> const float* {
                return reinterpret_cast<const float*>(base + i * stride);
            };
        };

        std::vector<Vertex>& vertices = out.vertices;
        std::vector<uint32_t>& indices = out.indices;

        // Pack all primitives into a single VBO/EBO
        for (const auto& mesh : model.meshes) {
            for (const auto& prim : mesh.primitives) {
                auto getPos = getAccessor(prim, "POSITION");
                auto getNorm = getAccessor(prim, "NORMAL");
                auto getUV = getAccessor(prim, "TEXCOORD_0");
                auto getTan = getAccessor(prim, "TANGENT");

                if (!getPos) continue;

                const auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
                const size_t vertOffset = vertices.size();
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
                                return reinterpret_cast<const uint16_t*>(idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i];
                            return reinterpret_cast<const uint32_t*>(idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset)[i];
                        };

                        for (size_t i = 0; i + 2 < idxAcc.count; i += 3) {
                            const uint32_t i0 = getIdx(i), i1 = getIdx(i + 1), i2 = getIdx(i + 2);

                            Vertex& v0 = vertices[vertOffset + i0];
                            Vertex& v1 = vertices[vertOffset + i1];
                            Vertex& v2 = vertices[vertOffset + i2];

                            const glm::vec3 edge1 = v1.position - v0.position;
                            const glm::vec3 edge2 = v2.position - v0.position;
                            const glm::vec2 dUV1 = v1.uv - v0.uv;
                            const glm::vec2 dUV2 = v2.uv - v0.uv;

                            const float det = dUV1.x * dUV2.y - dUV2.x * dUV1.y;
                            if (glm::abs(det) < 1e-6f) continue;

                            const float f = 1.0f / det;
                            const glm::vec3 T = f * (dUV2.y * edge1 - dUV1.y * edge2);

                            tangentAccum[i0] += T;
                            tangentAccum[i1] += T;
                            tangentAccum[i2] += T;
                        }
                    }

                    for (size_t i = 0; i < posAcc.count; ++i) {
                        Vertex& v = vertices[vertOffset + i];
                        const glm::vec3 N = v.normal;
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

                size_t primIndexCount = 0;
                if (prim.indices >= 0) {
                    const auto& idxAcc = model.accessors[prim.indices];
                    const auto& idxView = model.bufferViews[idxAcc.bufferView];
                    const auto& idxBuf = model.buffers[idxView.buffer];
                    const uint8_t* idxBase = idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset;

                    primIndexCount = idxAcc.count;
                    indices.reserve(indices.size() + idxAcc.count);
                    for (size_t i = 0; i < idxAcc.count; ++i) {
                        uint32_t index = (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                            ? static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(idxBase)[i])
                            : reinterpret_cast<const uint32_t*>(idxBase)[i];
                        indices.push_back(index + static_cast<uint32_t>(vertOffset));
                    }
                }

                PreparedSubMesh smr{};
                smr.indexOffset = static_cast<uint32_t>(indices.size() - primIndexCount);
                smr.indexCount = static_cast<uint32_t>(primIndexCount);

                if (prim.material >= 0 && prim.material < (int)model.materials.size()) {
                    const auto& mat = model.materials[prim.material];
                    const auto& pbr = mat.pbrMetallicRoughness;

                    smr.diffuseTex = prepareTex(pbr.baseColorTexture.index, true);          // sRGB
                    smr.normalTex = prepareTex(mat.normalTexture.index, false, true);      // linear, isNormalMap
                    smr.mrTex = prepareTex(pbr.metallicRoughnessTexture.index, false);     // linear
                    smr.occlusionTex = prepareTex(mat.occlusionTexture.index, false);      // linear
                    smr.emissiveTex = prepareTex(mat.emissiveTexture.index, false);        // linear
                }

                out.subMeshes.push_back(smr);
            }
        }

        // Bounding sphere: centre of the AABB, radius to the farthest vertex (tighter than the AABB's half-diagonal).
        if (!vertices.empty()) {
            glm::vec3 mn = vertices[0].position, mx = vertices[0].position;
            for (const Vertex& v : vertices) {
                mn = glm::min(mn, v.position);
                mx = glm::max(mx, v.position);
            }
            const glm::vec3 c = 0.5f * (mn + mx);
            float r2 = 0.0f;
            for (const Vertex& v : vertices) {
                const glm::vec3 d = v.position - c;
                r2 = std::max(r2, glm::dot(d, d));
            }
            out.boundsCenter = c;
            out.boundsRadius = std::sqrt(r2);
        }

        out.ok = true;
        return out;
    }

    // GL half: render thread only.
    inline MeshBuffer FinishMeshBuffer(PreparedMesh& prepared)
    {
        MeshBuffer buffer{};
        if (!prepared.ok)
            return buffer;

        const float anisotropy = RenderSettings::instance().getAnisotropy();

        std::vector<GLuint> textureIds(prepared.textures.size(), 0);
        for (size_t t = 0; t < prepared.textures.size(); ++t) {
            PreparedTexture& tex = prepared.textures[t];

            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);

            int w = tex.width, h = tex.height;
            for (size_t level = 0; level < tex.levels.size(); ++level) {
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), tex.internalFormat, w, h, 0,
                    tex.uploadFormat, GL_UNSIGNED_BYTE, tex.levels[level].data());
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
                // Each level's bytes are only needed for its upload: free them as we go, not at the end.
                std::vector<uint8_t>().swap(tex.levels[level]);
            }

            if (tex.compressed)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(tex.levels.size()) - 1);
            else
                glGenerateMipmap(GL_TEXTURE_2D);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, anisotropy);

            // At large world scale the GPU otherwise always picks mip 0 on normal maps, producing a specular dot.
            if (tex.isNormalMap)
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.0f);

            glBindTexture(GL_TEXTURE_2D, 0);
            textureIds[t] = id;
        }

        auto texId = [&](int index) -> GLuint { return index >= 0 ? textureIds[index] : 0; };
        for (const PreparedSubMesh& sm : prepared.subMeshes) {
            SubMeshRange smr{};
            smr.indexOffset = sm.indexOffset;
            smr.indexCount = sm.indexCount;
            smr.diffuseTex = texId(sm.diffuseTex);
            smr.normalTex = texId(sm.normalTex);
            smr.mrTex = texId(sm.mrTex);
            smr.occlusionTex = texId(sm.occlusionTex);
            smr.emissiveTex = texId(sm.emissiveTex);
            buffer.subMeshes.push_back(smr);
        }
        buffer.boundsCenter = prepared.boundsCenter;
        buffer.boundsRadius = prepared.boundsRadius;

        glGenVertexArrays(1, &buffer.VAO);
        glGenBuffers(1, &buffer.VBO);
        glGenBuffers(1, &buffer.EBO);

        glBindVertexArray(buffer.VAO);

        glBindBuffer(GL_ARRAY_BUFFER, buffer.VBO);
        glBufferData(GL_ARRAY_BUFFER, prepared.vertices.size() * sizeof(Vertex), prepared.vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer.EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, prepared.indices.size() * sizeof(uint32_t), prepared.indices.data(), GL_STATIC_DRAW);

        // location 0 — position
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
        glEnableVertexAttribArray(0);
        // location 1 — normal
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
        glEnableVertexAttribArray(1);
        // location 2 — uv
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
        glEnableVertexAttribArray(2);
        // location 3 — tangent (vec4, w = bitangent handedness)
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, tangent));
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);

        return buffer;
    }
}
