#ifndef GPU_MEMORY_LOG_HPP
#define GPU_MEMORY_LOG_HPP

#include "OpenGL/OpenGLIncludes.hpp"
#include "Utils/Debug/Debug.hpp"

// Logs the free video memory where the driver reports it (AMD: GL_ATI_meminfo, NVIDIA: GL_NVX_gpu_memory_info; nothing
// otherwise). Called when a world is set up and torn down, so a leak across matches, or the peak while two worlds'
// render targets coexist during a scene switch, shows in the log.
inline void LogGPUMemory(const char* when)
{
    if (GLEW_ATI_meminfo) {
        GLint tex[4] = {}, vbo[4] = {};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, tex);
        glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, vbo);
        Debug::Info("GPUMemory") << when << ": free for textures " << tex[0] / 1024 << " MB, for buffers "
            << vbo[0] / 1024 << " MB\n";
    }
    else if (GLEW_NVX_gpu_memory_info) {
        GLint total = 0, avail = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
        Debug::Info("GPUMemory") << when << ": " << avail / 1024 << " / " << total / 1024 << " MB free\n";
    }
}

#endif // GPU_MEMORY_LOG_HPP
