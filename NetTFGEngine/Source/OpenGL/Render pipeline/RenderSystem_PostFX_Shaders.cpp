#include "RenderSystem.hpp"

// =====================================================
//  CompileSSAOShaders
// =====================================================
void RenderSystem::CompileSSAOShaders()
{
    const char* quadVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* ssaoFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out float FragOcclusion;

        uniform sampler2D uDepthTex;
        uniform sampler2D uNoiseTex;
        uniform mat4      uProjection;
        uniform mat4      uInvProj;
        uniform vec3      uSamples[64];
        uniform int       uKernelSize;
        uniform float     uRadius;
        uniform float     uBias;
        uniform float     uPower;
        uniform ivec2     uScreenSize;

        vec3 ReconstructViewPos(vec2 uv, float depth)
        {
            vec4 ndcPos  = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
            vec4 viewPos = uInvProj * ndcPos;
            return viewPos.xyz / viewPos.w;
        }

        void main()
        {
            float depth = texture(uDepthTex, vUV).r;
            if (depth >= 0.9999) { FragOcclusion = 1.0; return; }

            vec3 fragPos   = ReconstructViewPos(vUV, depth);
            vec3 normal    = normalize(cross(dFdx(fragPos), dFdy(fragPos)));
            vec2 noiseScale = vec2(uScreenSize) / 4.0;
            vec3 randomVec  = normalize(texture(uNoiseTex, vUV * noiseScale).xyz);

            vec3 tangent   = normalize(randomVec - normal * dot(randomVec, normal));
            vec3 bitangent = cross(normal, tangent);
            mat3 TBN       = mat3(tangent, bitangent, normal);

            float occlusion = 0.0;
            for (int i = 0; i < uKernelSize; ++i) {
                vec3 samplePos = fragPos + TBN * uSamples[i] * uRadius;

                vec4 offset = uProjection * vec4(samplePos, 1.0);
                offset.xyz /= offset.w;
                offset.xyz  = offset.xyz * 0.5 + 0.5;

                float sampleDepth = texture(uDepthTex, offset.xy).r;
                vec3  sampleView  = ReconstructViewPos(offset.xy, sampleDepth);
                float rangeCheck  = smoothstep(0.0, 1.0,
                    uRadius / abs(fragPos.z - sampleView.z + 1e-5));

                occlusion += (sampleView.z >= samplePos.z + uBias ? 1.0 : 0.0)
                             * rangeCheck;
            }

            occlusion = 1.0 - (occlusion / float(uKernelSize));
            FragOcclusion = pow(occlusion, uPower);
        }
    )GLSL";

    const char* blurFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out float FragOcclusion;
        uniform sampler2D uSSAOTex;

        void main()
        {
            vec2  texelSize = 1.0 / vec2(textureSize(uSSAOTex, 0));
            float result    = 0.0;
            for (int x = -2; x <= 1; ++x)
                for (int y = -2; y <= 1; ++y)
                    result += texture(uSSAOTex,
                        vUV + vec2(float(x), float(y)) * texelSize).r;
            FragOcclusion = result / 16.0;
        }
    )GLSL";

    m_ssaoShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                     CompileStage(GL_FRAGMENT_SHADER, ssaoFrag) });
    m_ssaoBlurShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                     CompileStage(GL_FRAGMENT_SHADER, blurFrag) });

    // Pre-generate hemisphere sample kernel and upload once.
    glUseProgram(m_ssaoShader);
    srand(1337);
    for (int i = 0; i < 64; ++i) {
        glm::vec3 sample(
            (float)rand() / RAND_MAX * 2.0f - 1.0f,
            (float)rand() / RAND_MAX * 2.0f - 1.0f,
            (float)rand() / RAND_MAX          // z >= 0 -> hemisphere
        );
        sample = glm::normalize(sample);
        sample *= (float)rand() / RAND_MAX;

        float scale = (float)i / 64.0f;
        scale = 0.1f + scale * scale * 0.9f;
        sample *= scale;

        std::string uni = "uSamples[" + std::to_string(i) + "]";
        glUniform3fv(glGetUniformLocation(m_ssaoShader, uni.c_str()),
            1, glm::value_ptr(sample));
    }
    glUseProgram(0);
}

// =====================================================
//  CompileBloomShaders
// =====================================================
void RenderSystem::CompileBloomShaders()
{
    const char* quadVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* threshFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uHDRBuffer;
        uniform float     uThreshold;

        void main()
        {
            vec3  color  = texture(uHDRBuffer, vUV).rgb;
            float lum    = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float weight = smoothstep(uThreshold - 0.1, uThreshold + 0.1, lum);
            FragColor = vec4(color * weight, 1.0);
        }
    )GLSL";

    const char* kawaseFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uBloomTex;
        uniform int       uIteration;

        void main()
        {
            vec2  texelSize = 1.0 / vec2(textureSize(uBloomTex, 0));
            float offset    = float(uIteration) + 0.5;
            vec4  sum = vec4(0.0);
            sum += texture(uBloomTex, vUV + vec2(-offset, -offset) * texelSize);
            sum += texture(uBloomTex, vUV + vec2( offset, -offset) * texelSize);
            sum += texture(uBloomTex, vUV + vec2(-offset,  offset) * texelSize);
            sum += texture(uBloomTex, vUV + vec2( offset,  offset) * texelSize);
            FragColor = sum * 0.25;
        }
    )GLSL";

    m_bloomThreshShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                        CompileStage(GL_FRAGMENT_SHADER, threshFrag) });
    m_bloomKawaseShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                        CompileStage(GL_FRAGMENT_SHADER, kawaseFrag) });
}

// =====================================================
//  CompileSSRShader
// =====================================================
void RenderSystem::CompileSSRShader()
{
    const char* quadVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* ssrFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uDepthTex;
        uniform sampler2D uHDRColor;
        uniform mat4      uProjection;
        uniform mat4      uInvProj;
        uniform mat4      uView;
        uniform mat4      uInvView;
        uniform int       uMaxSteps;
        uniform float     uMaxDistance;
        uniform float     uStepSize;
        uniform int       uBinarySteps;
        uniform float     uRoughnessCutoff;
        uniform float     uFadeDistance;

        vec3 ViewPosFromDepth(vec2 uv, float depth)
        {
            vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
            vec4 vp  = uInvProj * ndc;
            return vp.xyz / vp.w;
        }

        vec2 ProjectToUV(vec3 vsPos)
        {
            vec4 clip = uProjection * vec4(vsPos, 1.0);
            clip.xyz /= clip.w;
            return clip.xy * 0.5 + 0.5;
        }

        void main()
        {
            float depth = texture(uDepthTex, vUV).r;
            if (depth >= 0.9999) { FragColor = vec4(0.0); return; }

            vec3  vsPos    = ViewPosFromDepth(vUV, depth);
            vec3  vsNormal = normalize(cross(dFdx(vsPos), dFdy(vsPos)));
            float roughness = 0.0;
            if (roughness > uRoughnessCutoff) { FragColor = vec4(0.0); return; }

            vec3 vsRefl = reflect(normalize(-vsPos), vsNormal);
            vec3 rayPos = vsPos;
            vec3 rayStep = vsRefl * uStepSize;
            bool hit = false;
            vec2 hitUV = vec2(0.0);

            for (int i = 0; i < uMaxSteps; ++i) {
                rayPos += rayStep;
                if (-rayPos.z > uMaxDistance) break;
                vec2 sUV = ProjectToUV(rayPos);
                if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) break;
                vec3 scenePos = ViewPosFromDepth(sUV, texture(uDepthTex, sUV).r);
                if (rayPos.z < scenePos.z) {
                    vec3 lo = rayPos - rayStep, hi = rayPos;
                    for (int b = 0; b < uBinarySteps; ++b) {
                        vec3 mid = (lo + hi) * 0.5;
                        vec2 mUV = ProjectToUV(mid);
                        if (mid.z < ViewPosFromDepth(mUV, texture(uDepthTex, mUV).r).z) hi = mid;
                        else lo = mid;
                    }
                    hitUV = ProjectToUV((lo + hi) * 0.5);
                    hit = true;
                    break;
                }
            }

            if (!hit) { FragColor = vec4(0.0); return; }

            vec2  edgeDist = min(hitUV, 1.0 - hitUV);
            float edgeFade = smoothstep(0.0, uFadeDistance, edgeDist.x)
                           * smoothstep(0.0, uFadeDistance, edgeDist.y);
            float distFade = 1.0 - clamp(length(rayPos - vsPos) / uMaxDistance, 0.0, 1.0);
            float weight   = edgeFade * distFade;
            vec3  reflCol  = texture(uHDRColor, hitUV).rgb;
            FragColor = vec4(reflCol * weight, weight);
        }
    )GLSL";

    m_ssrShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                CompileStage(GL_FRAGMENT_SHADER, ssrFrag) });
}

// =====================================================
//  CompileFXAAShader  (Lottes FXAA 3.11 simplified)
// =====================================================
void RenderSystem::CompileFXAAShader()
{
    const char* quadVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* fxaaFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uLDRBuffer;
        uniform vec2      uRcpFrame;
        uniform float     uEdgeThresholdMin;
        uniform float     uEdgeThreshold;
        uniform float     uSubpixel;

        float Luma(vec3 rgb) { return dot(rgb, vec3(0.2126, 0.7152, 0.0722)); }

        void main()
        {
            vec3 rgbM = texture(uLDRBuffer, vUV).rgb;
            vec3 rgbN = texture(uLDRBuffer, vUV + vec2( 0.0,  1.0) * uRcpFrame).rgb;
            vec3 rgbS = texture(uLDRBuffer, vUV + vec2( 0.0, -1.0) * uRcpFrame).rgb;
            vec3 rgbE = texture(uLDRBuffer, vUV + vec2( 1.0,  0.0) * uRcpFrame).rgb;
            vec3 rgbW = texture(uLDRBuffer, vUV + vec2(-1.0,  0.0) * uRcpFrame).rgb;

            float lumaM = Luma(rgbM), lumaN = Luma(rgbN), lumaS = Luma(rgbS);
            float lumaE = Luma(rgbE), lumaW = Luma(rgbW);
            float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
            float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
            float range    = rangeMax - rangeMin;

            if (range < max(uEdgeThresholdMin, rangeMax * uEdgeThreshold))
            { FragColor = vec4(rgbM, 1.0); return; }

            vec3 rgbNW = texture(uLDRBuffer, vUV + vec2(-1.0,  1.0) * uRcpFrame).rgb;
            vec3 rgbNE = texture(uLDRBuffer, vUV + vec2( 1.0,  1.0) * uRcpFrame).rgb;
            vec3 rgbSW = texture(uLDRBuffer, vUV + vec2(-1.0, -1.0) * uRcpFrame).rgb;
            vec3 rgbSE = texture(uLDRBuffer, vUV + vec2( 1.0, -1.0) * uRcpFrame).rgb;

            float lumaNW = Luma(rgbNW), lumaNE = Luma(rgbNE);
            float lumaSW = Luma(rgbSW), lumaSE = Luma(rgbSE);

            float lumaAvg = ((lumaN + lumaS + lumaE + lumaW) * 2.0
                          + (lumaNW + lumaNE + lumaSW + lumaSE)) / 12.0;
            float subBlend = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
            subBlend = subBlend * subBlend * uSubpixel;

            float edgeH = abs(-2.0*lumaW + lumaNW + lumaSW)
                        + abs(-2.0*lumaM + lumaN  + lumaS ) * 2.0
                        + abs(-2.0*lumaE + lumaNE + lumaSE);
            float edgeV = abs(-2.0*lumaN + lumaNW + lumaNE)
                        + abs(-2.0*lumaM + lumaW  + lumaE ) * 2.0
                        + abs(-2.0*lumaS + lumaSW + lumaSE);
            bool isHoriz = edgeH >= edgeV;

            float stepLen  = isHoriz ? uRcpFrame.y : uRcpFrame.x;
            float luma1    = isHoriz ? lumaS : lumaW;
            float luma2    = isHoriz ? lumaN : lumaE;
            bool  side1    = abs(luma1 - lumaM) >= abs(luma2 - lumaM);
            float gradStep = max(abs(luma1 - lumaM), abs(luma2 - lumaM)) * 0.25;
            float lumaEdge = side1 ? (luma1+lumaM)*0.5 : (luma2+lumaM)*0.5;

            vec2 offStep = isHoriz ? vec2(uRcpFrame.x, 0.0) : vec2(0.0, uRcpFrame.y);
            vec2 offPerp = isHoriz ? vec2(0.0, stepLen*(side1?-1.0:1.0))
                                   : vec2(stepLen*(side1?-1.0:1.0), 0.0);

            vec2 uv1 = vUV + offPerp - offStep;
            vec2 uv2 = vUV + offPerp + offStep;
            float done1=0.0, done2=0.0, end1=0.0, end2=0.0;

            for (int i = 0; i < 8; ++i) {
                if (done1 < 0.5) { end1 = Luma(texture(uLDRBuffer,uv1).rgb)-lumaEdge; uv1-=offStep; }
                if (done2 < 0.5) { end2 = Luma(texture(uLDRBuffer,uv2).rgb)-lumaEdge; uv2+=offStep; }
                done1 = step(gradStep, abs(end1));
                done2 = step(gradStep, abs(end2));
                if (done1>0.5 && done2>0.5) break;
            }

            float dist1 = isHoriz?(vUV.x-(uv1.x+offStep.x)):(vUV.y-(uv1.y+offStep.y));
            float dist2 = isHoriz?((uv2.x-offStep.x)-vUV.x):((uv2.y-offStep.y)-vUV.y);
            bool  sideF = dist1 < dist2;
            float edgeBlend = 0.5 - min(dist1,dist2)/(dist1+dist2);
            bool  correctDir = (sideF?end1:end2) < 0.0 != (lumaM < lumaEdge);
            float finalBlend = max(subBlend, correctDir ? edgeBlend : 0.0);

            FragColor = vec4(texture(uLDRBuffer, vUV + offPerp*finalBlend).rgb, 1.0);
        }
    )GLSL";

    m_fxaaShader = LinkProgram({ CompileStage(GL_VERTEX_SHADER, quadVert),
                                 CompileStage(GL_FRAGMENT_SHADER, fxaaFrag) });
}