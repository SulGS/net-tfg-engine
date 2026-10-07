#ifndef GPU_MEMORY_LOG_HPP
#define GPU_MEMORY_LOG_HPP

#include "OpenGL/OpenGLIncludes.hpp"
#include "Utils/Debug/Debug.hpp"
#include <cstring>

// GLEW's GLEW_xxx flags come from glGetString(GL_EXTENSIONS), which a core profile doesn't answer: ask glGetStringi.
inline bool HasGLExtension(const char* name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        if (ext && std::strcmp(ext, name) == 0) return true;
    }
    return false;
}

// Logs the free video memory where the driver reports it (AMD: GL_ATI_meminfo, NVIDIA: GL_NVX_gpu_memory_info; nothing
// otherwise). Called when a world is set up and torn down, so a leak across matches, or the peak while two worlds'
// render targets coexist during a scene switch, shows in the log.
inline void LogGPUMemory(const char* when)
{
    static const bool ati = HasGLExtension("GL_ATI_meminfo");
    static const bool nvx = HasGLExtension("GL_NVX_gpu_memory_info");
    if (ati) {
        GLint tex[4] = {}, vbo[4] = {};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, tex);
        glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, vbo);
        Debug::Info("GPUMemory") << when << ": free for textures " << tex[0] / 1024 << " MB, for buffers "
            << vbo[0] / 1024 << " MB\n";
    }
    else if (nvx) {
        GLint total = 0, avail = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
        Debug::Info("GPUMemory") << when << ": " << avail / 1024 << " / " << total / 1024 << " MB free\n";
    }
    else {
        static bool told = false;
        if (!told) Debug::Info("GPUMemory") << "The driver exposes no free-memory query (ATI_meminfo / NVX)\n";
        told = true;
    }
}

#endif // GPU_MEMORY_LOG_HPP
