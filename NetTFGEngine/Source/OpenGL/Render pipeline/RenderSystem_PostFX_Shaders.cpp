#include "RenderSystem.hpp"

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