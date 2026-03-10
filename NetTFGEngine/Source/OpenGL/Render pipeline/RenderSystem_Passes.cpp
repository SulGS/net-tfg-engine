#include "RenderSystem.hpp"

// =====================================================
//  UploadLights
// =====================================================
void RenderSystem::UploadLights(EntityManager& em)
{
    std::vector<GPUPointLight> lights;
    lights.reserve(MAX_LIGHTS);

    auto q = em.CreateQuery<PointLightComponent, Transform>();
    for (auto [entity, light, xform] : q) {
        if ((int)lights.size() >= MAX_LIGHTS) break;
        GPUPointLight gl;
        gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
        gl.colorIntensity = glm::vec4(light->color, light->intensity);
        lights.push_back(gl);
    }

    m_lightCount = (int)lights.size();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
        sizeof(GPUPointLight) * m_lightCount, lights.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// =====================================================
//  ShadowPass
// =====================================================
void RenderSystem::ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    const auto& rs = RenderSettings::instance();

    std::vector<GPUShadowData> shadowDataVec;

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glViewport(0, 0, m_shadowRes, m_shadowRes);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(rs.getShadowBiasFactor(), rs.getShadowBiasUnits());

    glUseProgram(m_shadowShader);

    int  shadowIdx = 0;
    auto lightQuery = em.CreateQuery<PointLightComponent, Transform>();

    for (auto [entity, light, xform] : lightQuery) {
        if (shadowIdx >= MAX_SHADOW_LIGHTS) break;

        glClear(GL_DEPTH_BUFFER_BIT); // must clear per light

        glm::vec3 pos = xform->getPosition();
        float     farPlane = light->radius;

        glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f,
            rs.getShadowNearPlane(), farPlane);

        glm::mat4 views[6] = {
            shadowProj * glm::lookAt(pos, pos + glm::vec3(1, 0, 0), glm::vec3(0,-1, 0)),
            shadowProj * glm::lookAt(pos, pos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
            shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),
            shadowProj * glm::lookAt(pos, pos + glm::vec3(0,-1, 0), glm::vec3(0, 0,-1)),
            shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0, 1), glm::vec3(0,-1, 0)),
            shadowProj * glm::lookAt(pos, pos + glm::vec3(0, 0,-1), glm::vec3(0,-1, 0)),
        };

        for (int f = 0; f < 6; f++) {
            std::string uni = "uLightSpaceMatrices[" + std::to_string(f) + "]";
            glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, uni.c_str()),
                1, GL_FALSE, glm::value_ptr(views[f]));
        }
        glUniform3fv(glGetUniformLocation(m_shadowShader, "uLightPos"),
            1, glm::value_ptr(pos));
        glUniform1f(glGetUniformLocation(m_shadowShader, "uFarPlane"), farPlane);
        glUniform1i(glGetUniformLocation(m_shadowShader, "uCubeArrayLayer"), shadowIdx * 6);

        for (auto [me, meshC, xf] : meshQuery) {
            if (!meshC->enabled || !meshC->mesh) continue;
            glUniformMatrix4fv(glGetUniformLocation(m_shadowShader, "uModel"),
                1, GL_FALSE, glm::value_ptr(xf->getModelMatrix()));
            meshC->mesh->drawDepthOnly(glm::mat4(1.0f), m_shadowShader);
        }

        GPUShadowData sd;
        for (int f = 0; f < 6; f++) sd.lightSpaceMatrices[f] = views[f];
        sd.lightIndex = shadowIdx;
        sd.farPlane = farPlane;
        shadowDataVec.push_back(sd);
        shadowIdx++;
    }

    m_shadowCount = shadowIdx;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
        sizeof(GPUShadowData) * m_shadowCount, shadowDataVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
}

// =====================================================
//  DepthPrePass
// =====================================================
void RenderSystem::DepthPrePass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view,
    const glm::mat4& projection)
{
    glViewport(0, 0, m_screenW, m_screenH); // restore after shadow pass
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glUseProgram(m_depthShader);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled) continue;
        Mesh* mesh = meshC->mesh.get();
        if (!mesh) continue;

        glm::mat4 mvp = projection * view * transform->getModelMatrix();
        mesh->drawDepthOnly(mvp, m_depthShader);
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Ensure depth writes are visible to the compute shader that samples m_depthTex.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

// =====================================================
//  LightCullPass
// =====================================================
void RenderSystem::LightCullPass(const glm::mat4& view,
    const glm::mat4& projection)
{
    glUseProgram(m_lightCullShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrDepthTex); // shared with depthFBO and hdrFBO
    glUniform1i(glGetUniformLocation(m_lightCullShader, "uDepthMap"), 0);

    glm::mat4 invProj = glm::inverse(projection); // pre-invert once on CPU
    glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uProjection"),
        1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uInvProjection"),
        1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(glGetUniformLocation(m_lightCullShader, "uView"),
        1, GL_FALSE, glm::value_ptr(view));
    glUniform1i(glGetUniformLocation(m_lightCullShader, "uLightCount"), m_lightCount);
    glUniform2i(glGetUniformLocation(m_lightCullShader, "uScreenSize"), m_screenW, m_screenH);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);

    glDispatchCompute((GLuint)m_tilesX, (GLuint)m_tilesY, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

// =====================================================
//  ShadingPass
// =====================================================
void RenderSystem::ShadingPass(EntityManager::Query<MeshComponent, Transform>& meshQuery,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPos)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_tileGridSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_shadowDataSSBO);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;

        glm::mat4 model = transform->getModelMatrix();
        meshC->mesh->bindMaterial(model, view, projection);

        if (Material* mat = meshC->mesh->getMaterial()) {
            mat->setIVec2("uScreenSize", glm::ivec2(m_screenW, m_screenH));
            mat->setVec3("uCameraPos", cameraPos);
            mat->setInt("uShadowCubeArray", 5);
            mat->setInt("uShadowCount", m_shadowCount);
        }
        meshC->mesh->draw();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}