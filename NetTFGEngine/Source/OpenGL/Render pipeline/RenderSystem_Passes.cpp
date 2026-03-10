#include "RenderSystem.hpp"

// =====================================================
//  ShadowPass
// =====================================================
void RenderSystem::ShadowPass(EntityManager& em, EntityManager::Query<MeshComponent, Transform>& meshQuery)
{
    const auto& rs = RenderSettings::instance();

    // Build light list and shadow data in a single pass over lights
    // so that sd.lightIndex stores the absolute light-buffer slot index,
    // matching what the fragment shader reads from uLightCount lights[].
    std::vector<GPUPointLight>  lightVec;
    std::vector<GPUShadowData>  shadowDataVec;
    lightVec.reserve(MAX_LIGHTS);

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
        if ((int)lightVec.size() >= MAX_LIGHTS) break;

        int lightBufIdx = (int)lightVec.size();

        // Accumulate into the light SSBO buffer
        GPUPointLight gl;
        gl.posRadius = glm::vec4(xform->getPosition(), light->radius);
        gl.colorIntensity = glm::vec4(light->color, light->intensity);
        lightVec.push_back(gl);

        if (shadowIdx >= MAX_SHADOW_LIGHTS) continue;

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
        sd.lightIndex = lightBufIdx; // absolute index into lights[] SSBO
        sd.farPlane = farPlane;
        shadowDataVec.push_back(sd);
        shadowIdx++;
    }

    m_lightCount = (int)lightVec.size();
    m_shadowCount = shadowIdx;

    // Upload light SSBO
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
    if (m_lightCount > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUPointLight) * m_lightCount, lightVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Upload shadow SSBO
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowDataSSBO);
    if (m_shadowCount > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
            sizeof(GPUShadowData) * m_shadowCount, shadowDataVec.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screenW, m_screenH);
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

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO);      // binding 0 — lights[]
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_shadowDataSSBO); // binding 1 — shadows[]

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_shadowCubeArray);

    for (auto [entity, meshC, transform] : meshQuery) {
        if (!meshC->enabled || !meshC->mesh) continue;

        glm::mat4 model = transform->getModelMatrix();
        meshC->mesh->bindMaterial(model, view, projection);

        if (Material* mat = meshC->mesh->getMaterial()) {
            mat->setVec3("uCameraPos", cameraPos);
            mat->setInt("uShadowCubeArray", 5);
            mat->setInt("uShadowCount", m_shadowCount);
            mat->setInt("uLightCount", m_lightCount);
        }
        meshC->mesh->draw();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}