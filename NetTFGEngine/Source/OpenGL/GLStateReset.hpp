#ifndef GL_STATE_RESET_HPP
#define GL_STATE_RESET_HPP

#include "OpenGL/OpenGLIncludes.hpp"
#include <algorithm>

// Puts every GL binding point a world's renderer uses back to 0: textures on every unit and target, samplers, image
// units, indexed SSBO/UBO bindings, program, VAO, buffers, framebuffer. Called on the render thread before a world's GL
// objects are deleted. Deleting a bound object is meant to unbind it, but an old AMD driver (Radeon 520, 21.19) kept a
// stale reference after leaving a match and crashed (null write inside the driver) on the next scene's first draw.
inline void ResetGLBindings()
{
    GLint units = 0, images = 0, ssbos = 0, ubos = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &images);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &ssbos);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &ubos);

    static const GLenum kTargets[] = {
        GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_ARRAY,
        GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_3D,
    };
    for (GLint u = 0; u < std::min(units, 32); ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        for (GLenum t : kTargets) glBindTexture(t, 0);
        glBindSampler(u, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    for (GLint i = 0; i < std::min(images, 8); ++i)
        glBindImageTexture(i, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    for (GLint i = 0; i < std::min(ssbos, 16); ++i)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, 0);
    for (GLint i = 0; i < std::min(ubos, 16); ++i)
        glBindBufferBase(GL_UNIFORM_BUFFER, i, 0);

    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

#endif // GL_STATE_RESET_HPP
