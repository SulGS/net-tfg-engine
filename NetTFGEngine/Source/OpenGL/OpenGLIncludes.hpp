#ifndef OPENGLINCLUDES
#define OPENGLINCLUDES

#ifdef NETTFG_HEADLESS
#error "OpenGL/OpenGLIncludes.hpp is client-only (OpenGL/GLFW/OpenAL/FreeType/SOIL): headless targets (NetTFGEngineCore, GameServer) must not include it"
#endif
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#if defined(_WIN32) || defined(_WIN64)
#include <gl/gl.h>
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#endif