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
//  Gamma correction applied after both.
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

        uniform float uExposure;
        uniform bool  uFilmicEnabled;
        uniform float uGamma;

        uniform bool  uBloomEnabled;
        uniform float uBloomStrength;

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