#include "RenderSystem.hpp"

// =====================================================
//  CompileShadowShader
// =====================================================
void RenderSystem::CompileShadowShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 uModel;
        void main() { gl_Position = uModel * vec4(aPos, 1.0); }
    )GLSL";

    const char* geom = R"GLSL(
        #version 430 core
        layout(triangles) in;
        layout(triangle_strip, max_vertices = 18) out;

        uniform mat4 uLightSpaceMatrices[6];
        uniform int  uCubeArrayLayer;

        out vec4 gFragPos;

        void main() {
            for (int face = 0; face < 6; face++) {
                gl_Layer = uCubeArrayLayer + face;
                for (int v = 0; v < 3; v++) {
                    gFragPos    = gl_in[v].gl_Position;
                    gl_Position = uLightSpaceMatrices[face] * gFragPos;
                    EmitVertex();
                }
                EndPrimitive();
            }
        }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec4  gFragPos;
        uniform vec3  uLightPos;
        uniform float uFarPlane;
        void main() {
            gl_FragDepth = length(gFragPos.xyz - uLightPos) / uFarPlane;
        }
    )GLSL";

    m_shadowShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_GEOMETRY_SHADER, geom),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}

// =====================================================
//  CompileTonemapShader
//  Reinhard and Filmic (Uncharted 2 / Hable) operators.
//  Gamma correction applied after both. Composites bloom.
// =====================================================
void RenderSystem::CompileTonemapShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uHDRBuffer;
        uniform sampler2D uBloomTex;
        uniform bool      uBloomEnabled;
        uniform float     uBloomStrength;

        uniform float uExposure;
        uniform bool  uFilmicEnabled;
        uniform float uGamma;
        uniform float uA, uB, uC, uD, uE, uF, uW;

        vec3 FilmicCurve(vec3 x)
        {
            return ((x * (uA * x + uC * uB) + uD * uE)
                  / (x * (uA * x + uB)       + uD * uF))
                 - uE / uF;
        }

        void main()
        {
            vec3 hdrColor = texture(uHDRBuffer, vUV).rgb;

            if (uBloomEnabled)
                hdrColor += texture(uBloomTex, vUV).rgb * uBloomStrength;

            hdrColor *= uExposure;

            vec3 mapped;
            if (uFilmicEnabled) {
                vec3 whiteScale = vec3(1.0) / FilmicCurve(vec3(uW));
                mapped = FilmicCurve(hdrColor) * whiteScale;
            } else {
                mapped = hdrColor / (hdrColor + vec3(1.0));
            }

            mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / uGamma));
            FragColor = vec4(mapped, 1.0);
        }
    )GLSL";

    m_tonemapShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}
// =====================================================
//  CompileBloomShaders
//  Two programs:
//    Threshold � extracts bright regions above uThreshold
//    Kawase    � single-pass Kawase blur; run multiple times
//               with increasing uIteration offsets
// =====================================================
void RenderSystem::CompileBloomShaders()
{
    const char* fullscreenVert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    // ---- Threshold ----
    const char* threshFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uHDRBuffer;
        uniform float     uThreshold;
        void main()
        {
            vec3 color = texture(uHDRBuffer, vUV).rgb;
            float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
            FragColor = vec4((brightness > uThreshold) ? color : vec3(0.0), 1.0);
        }
    )GLSL";

    m_bloomThreshShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   fullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, threshFrag)
        });

    // ---- Kawase blur ----
    const char* kawaseFrag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uTex;
        uniform vec2      uTexelSize; // 1.0 / resolution
        uniform int       uIteration;
        void main()
        {
            float offset = float(uIteration) + 0.5;
            vec3 sum = vec3(0.0);
            sum += texture(uTex, vUV + vec2(-offset, -offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2( offset, -offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2(-offset,  offset) * uTexelSize).rgb;
            sum += texture(uTex, vUV + vec2( offset,  offset) * uTexelSize).rgb;
            FragColor = vec4(sum * 0.25, 1.0);
        }
    )GLSL";

    m_bloomKawaseShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   fullscreenVert),
        CompileStage(GL_FRAGMENT_SHADER, kawaseFrag)
        });
}

// =====================================================
//  CompileFXAAShader
//  FXAA 3.11 quality implementation in a single pass.
// =====================================================
void RenderSystem::CompileFXAAShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in  vec2 vUV;
        out vec4 FragColor;

        uniform sampler2D uLDRBuffer;
        uniform vec2      uTexelSize;   // 1.0 / resolution
        uniform float     uSubpix;
        uniform float     uEdgeThreshold;
        uniform float     uEdgeThresholdMin;

        float Luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

        void main()
        {
            vec2 uv = vUV;

            // Sample centre and cardinal neighbours
            vec3  rgbM  = texture(uLDRBuffer, uv).rgb;
            float lumaM = Luma(rgbM);
            float lumaN = Luma(texture(uLDRBuffer, uv + vec2( 0, -1) * uTexelSize).rgb);
            float lumaS = Luma(texture(uLDRBuffer, uv + vec2( 0,  1) * uTexelSize).rgb);
            float lumaW = Luma(texture(uLDRBuffer, uv + vec2(-1,  0) * uTexelSize).rgb);
            float lumaE = Luma(texture(uLDRBuffer, uv + vec2( 1,  0) * uTexelSize).rgb);

            float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
            float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
            float range    = rangeMax - rangeMin;

            // Skip pixels below contrast threshold
            if (range < max(uEdgeThresholdMin, rangeMax * uEdgeThreshold)) {
                FragColor = vec4(rgbM, 1.0);
                return;
            }

            // Diagonal neighbours
            float lumaNW = Luma(texture(uLDRBuffer, uv + vec2(-1, -1) * uTexelSize).rgb);
            float lumaNE = Luma(texture(uLDRBuffer, uv + vec2( 1, -1) * uTexelSize).rgb);
            float lumaSW = Luma(texture(uLDRBuffer, uv + vec2(-1,  1) * uTexelSize).rgb);
            float lumaSE = Luma(texture(uLDRBuffer, uv + vec2( 1,  1) * uTexelSize).rgb);

            // Edge direction
            float edgeH = abs(lumaNW + 2.0*lumaN + lumaNE - lumaSW - 2.0*lumaS - lumaSE);
            float edgeV = abs(lumaNW + 2.0*lumaW + lumaSW - lumaNE - 2.0*lumaE - lumaSE);
            bool  isHorizontal = edgeH >= edgeV;

            // Sub-pixel blend
            float lumaAvg = (2.0*(lumaN+lumaS+lumaW+lumaE) +
                             lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
            float subpixBlend = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
            subpixBlend = smoothstep(0.0, 1.0, subpixBlend) * uSubpix;

            // Blend along detected edge direction
            vec2 blendDir = isHorizontal ? vec2(0.0, uTexelSize.y)
                                         : vec2(uTexelSize.x, 0.0);
            vec3 rgbBlend = 0.5 * (rgbM + texture(uLDRBuffer, uv + blendDir).rgb);

            FragColor = vec4(mix(rgbM, rgbBlend, subpixBlend), 1.0);
        }
    )GLSL";

    m_fxaaShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}


// =====================================================
//  CompileGBufferShader
//  Geometry pre-pass: outputs view-space normal (xyz) +
//  perceptual roughness (w) into a single RGBA16F attachment.
//  Reads the same vertex attributes and uniforms as ggx.vert
//  so no extra VAO layout is needed.
// =====================================================
void RenderSystem::CompileGBufferShader()
{
    const char* vert = R"GLSL(
        #version 430 core
        layout(location = 0) in vec3 aPos;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec2 aUV;
        layout(location = 3) in vec4 aTangent; // xyz = tangent, w = bitangent sign

        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;

        out vec2 vUV;
        out mat3 vViewTBN;

        void main()
        {
            mat3 viewNormalMatrix = mat3(transpose(inverse(uView * uModel)));

            vec3 vN = normalize(viewNormalMatrix * aNormal);
            vec3 vT = normalize(viewNormalMatrix * aTangent.xyz);
            vT      = normalize(vT - dot(vT, vN) * vN); // Gram-Schmidt
            vec3 vB = cross(vN, vT) * aTangent.w;
            vViewTBN = mat3(vT, vB, vN);

            vUV         = aUV;
            gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
        }
    )GLSL";

    const char* frag = R"GLSL(
        #version 430 core
        in vec2 vUV;
        in mat3 vViewTBN;

        layout(location = 0) out vec4 FragNormalRoughness; // xyz=normal, w=roughness (backward-compat)
        layout(location = 1) out vec4 FragRoughness;       // r=roughness
        layout(location = 2) out vec4 FragMetalness;       // r=metalness

        uniform sampler2D uNormalTex; // unit 1 — tangent-space normal map
        uniform sampler2D uMRTex;     // unit 2 — G=roughness, B=metallic

        void main()
        {
            vec3 tangentN   = texture(uNormalTex, vUV).rgb * 2.0 - 1.0;
            vec3 viewNormal = normalize(vViewTBN * tangentN);

            vec2 mr = texture(uMRTex, vUV).gb; // g=roughness, b=metallic
            float perceptualRoughness = clamp(mr.x, 0.045, 1.0);
            float metalness           = clamp(mr.y, 0.0,   1.0);

            FragNormalRoughness = vec4(viewNormal, perceptualRoughness);
            FragRoughness       = vec4(perceptualRoughness, 0.0, 0.0, 1.0);
            FragMetalness       = vec4(metalness,           0.0, 0.0, 1.0);
        }
    )GLSL";

    m_gbufferShader = LinkProgram({
        CompileStage(GL_VERTEX_SHADER,   vert),
        CompileStage(GL_FRAGMENT_SHADER, frag)
        });
}