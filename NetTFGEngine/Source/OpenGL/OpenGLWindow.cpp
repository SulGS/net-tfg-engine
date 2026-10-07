#include "OpenGLWindow.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include "Utils/Debug/Debug.hpp"

// Driver-reported GL errors / undefined behaviour into the engine log (otherwise silent: NVIDIA tolerates things other
// vendors reject, and the symptom shows up far from the call). Synchronous, so it fires inside the offending call on the
// render thread. Each message id is logged a few times only: a per-frame error would otherwise flood the log.
static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei /*length*/, const GLchar* message, const void* /*user*/)
{
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type != GL_DEBUG_TYPE_ERROR && type != GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR && severity == GL_DEBUG_SEVERITY_LOW) return;

    static std::mutex m;
    static std::unordered_map<GLuint, int> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        int& n = seen[id];
        if (++n > 5) return;
    }

    const char* t = type == GL_DEBUG_TYPE_ERROR ? "ERROR"
        : type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR ? "UNDEFINED"
        : type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR ? "DEPRECATED"
        : type == GL_DEBUG_TYPE_PORTABILITY ? "PORTABILITY"
        : type == GL_DEBUG_TYPE_PERFORMANCE ? "PERFORMANCE" : "OTHER";
    const char* sev = severity == GL_DEBUG_SEVERITY_HIGH ? "high" : severity == GL_DEBUG_SEVERITY_MEDIUM ? "medium" : "low";
    Debug::Warning("GL") << t << " (" << sev << ", id " << id << ", source 0x" << std::hex << source << std::dec
        << "): " << (message ? message : "") << "\n";
}

OpenGLWindow::OpenGLWindow(int width, int height, const std::string& title)
    : window(nullptr)
{
    initializeGLFW();

    window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(window);
    initializeGLEW();
    setupOpenGL();

    glfwSetWindowUserPointer(window, this);
    // Framebuffer resize callback � physical pixels, used for glViewport
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* win, int w, int h) {
        // Minimizing (e.g. Alt+Tab out of exclusive fullscreen) reports 0x0.
        // Keep the last real size: resizing the render targets to 0 leaves
        // every FBO incomplete and turns the aspect ratio into NaN.
        if (w <= 0 || h <= 0) return;

        auto* self = static_cast<OpenGLWindow*>(glfwGetWindowUserPointer(win));
        glViewport(0, 0, w, h);
        if (w != self->currentWidth || h != self->currentHeight) {
            self->currentWidth = w;
            self->currentHeight = h;
            self->resized = true;
        }
        });

    // Window size callback � logical pixels, matches glfwGetCursorPos space
    glfwSetWindowSizeCallback(window, [](GLFWwindow* win, int w, int h) {
        if (w <= 0 || h <= 0) return; // minimized, see framebuffer callback

        auto* self = static_cast<OpenGLWindow*>(glfwGetWindowUserPointer(win));
        self->logicalWidth = w;
        self->logicalHeight = h;
        });

    glfwGetFramebufferSize(window, &currentWidth, &currentHeight);
    glViewport(0, 0, currentWidth, currentHeight);

    glfwGetWindowSize(window, &logicalWidth, &logicalHeight);

    windowedWidth = logicalWidth;
    windowedHeight = logicalHeight;
    centerWindowedPosition();
    glfwSetWindowPos(window, windowedX, windowedY);
}

OpenGLWindow::~OpenGLWindow() {
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
}

void OpenGLWindow::swapBuffers() {
    glfwSwapBuffers(window);
}

void OpenGLWindow::pollEvents() {
    glfwPollEvents();
}

bool OpenGLWindow::isMinimized() const {
    return glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE;
}

bool OpenGLWindow::shouldClose() const {
    return glfwWindowShouldClose(window);
}

void OpenGLWindow::makeContextCurrent() {
    glfwMakeContextCurrent(window);
}

void OpenGLWindow::releaseContext() {
    glfwMakeContextCurrent(nullptr);
}

void OpenGLWindow::close() {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

WindowMode OpenGLWindow::getWindowMode() const {
    if (glfwGetWindowMonitor(window) != nullptr) return WindowMode::Fullscreen;
    if (!glfwGetWindowAttrib(window, GLFW_DECORATED)) return WindowMode::Borderless;
    return WindowMode::Windowed;
}

void OpenGLWindow::setWindowMode(WindowMode mode) {
    if (mode == getWindowMode()) return;

    // Remember where the window was before leaving Windowed, so it comes back to the same place regardless of how
    // many times Borderless/Fullscreen were toggled in between. The size is not read back: it is the setting.
    if (getWindowMode() == WindowMode::Windowed)
        glfwGetWindowPos(window, &windowedX, &windowedY);

    switch (mode) {
    case WindowMode::Windowed:
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window, nullptr, windowedX, windowedY, windowedWidth, windowedHeight, 0);
        break;

    case WindowMode::Borderless:
    {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode_ = glfwGetVideoMode(monitor);
        int monitorX, monitorY;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);

        // Drop exclusive fullscreen first (monitor = nullptr): GLFW only
        // honors GLFW_DECORATED changes on a windowed-mode window.
        glfwSetWindowMonitor(window, nullptr, monitorX, monitorY, mode_->width, mode_->height, 0);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        // Re-assert geometry: removing the border can shift the client area.
        glfwSetWindowPos(window, monitorX, monitorY);
        glfwSetWindowSize(window, mode_->width, mode_->height);
        break;
    }

    case WindowMode::Fullscreen:
    {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode_ = glfwGetVideoMode(monitor);
        // Restore decoration so a later direct switch back to Windowed looks right.
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode_->width, mode_->height, mode_->refreshRate);
        break;
    }
    }

    // glfwSetWindowMonitor recreates the swap chain on some drivers (exclusive fullscreen switches), silently
    // resetting the swap interval. Re-assert the configured VSync so it doesn't flip.
    glfwSwapInterval(vsyncEnabled ? 1 : 0);
}

void OpenGLWindow::setWindowedSize(int width, int height) {
    int monitorW, monitorH;
    getMonitorResolution(monitorW, monitorH);
    windowedWidth = std::clamp(width, 320, std::max(320, monitorW));
    windowedHeight = std::clamp(height, 240, std::max(240, monitorH));

    // A different size means the old position may no longer fit: re-centre either way.
    centerWindowedPosition();

    if (getWindowMode() == WindowMode::Windowed)
        glfwSetWindowMonitor(window, nullptr, windowedX, windowedY, windowedWidth, windowedHeight, 0);
}

void OpenGLWindow::centerWindowedPosition() {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    int areaX = 0, areaY = 0, areaW = windowedWidth, areaH = windowedHeight;
    if (monitor) glfwGetMonitorWorkarea(monitor, &areaX, &areaY, &areaW, &areaH);

    // Never above/left of the work area, so the title bar stays reachable even when the window is as big as the
    // monitor (it then spills past the bottom/right edge instead).
    windowedX = areaX + std::max(0, (areaW - windowedWidth) / 2);
    windowedY = areaY + std::max(0, (areaH - windowedHeight) / 2);
}

void OpenGLWindow::getMonitorResolution(int& width, int& height) {
    width = 1920;
    height = 1080;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) {
        width = mode->width;
        height = mode->height;
    }
}

std::vector<std::pair<int, int>> OpenGLWindow::getAvailableResolutions() {
    int monitorW, monitorH;
    getMonitorResolution(monitorW, monitorH);

    std::vector<std::pair<int, int>> result;
    int count = 0;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* modes = monitor ? glfwGetVideoModes(monitor, &count) : nullptr;
    for (int i = 0; i < count; ++i) {
        const std::pair<int, int> size(modes[i].width, modes[i].height);
        // Modes differ in refresh rate / bit depth too: keep each size once.
        if (size.first < 1024 || size.second < 576) continue;
        if (size.first > monitorW || size.second > monitorH) continue;
        if (std::find(result.begin(), result.end(), size) == result.end())
            result.push_back(size);
    }

    // The native size is what Borderless/Fullscreen show, so it must always be in the list.
    const std::pair<int, int> native(monitorW, monitorH);
    if (std::find(result.begin(), result.end(), native) == result.end())
        result.push_back(native);

    std::sort(result.begin(), result.end());
    return result;
}

int OpenGLWindow::getWidth()  const { return currentWidth; }
int OpenGLWindow::getHeight() const { return currentHeight; }
int OpenGLWindow::getLogicalWidth()  const { return logicalWidth; }
int OpenGLWindow::getLogicalHeight() const { return logicalHeight; }

bool OpenGLWindow::wasResized() {
    bool r = resized;
    resized = false; // consume the flag
    return r;
}

float OpenGLWindow::getAspectRatio() const {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    return h > 0 ? (float)w / (float)h : 1.0f;
}

void OpenGLWindow::initializeGLFW() {
    if (!glfwInit())
        throw std::runtime_error("Failed to initialize GLFW");

    // 4.3: the renderer needs SSBOs (lights, shadows, particles), compute (NIS), textureQueryLod and debug output, and
    // every shader is #version 430. Asking for 3.3 got exactly 3.3 on NVIDIA, which tolerates 4.3 features in it anyway;
    // other drivers don't have to.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // NETTFG_GL_DEBUG=1: debug context, so every driver reports errors through GLDebugCallback (non-debug contexts may
    // report fewer). Slower, so opt-in.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: read once at startup, portable to the Linux build
#endif
    const char* dbg = std::getenv("NETTFG_GL_DEBUG");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (dbg && dbg[0] == '1')
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    // No dragging the borders nor maximising: the size only changes through the window resolution setting.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
}

void OpenGLWindow::initializeGLEW() {
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        glfwTerminate();
        throw std::runtime_error("Failed to initialize GLEW");
    }
}

void OpenGLWindow::setVSync(bool enabled)
{
    vsyncEnabled = enabled;
    glfwSwapInterval(enabled ? 1 : 0);
}

void OpenGLWindow::setupOpenGL()
{
    auto glStr = [](GLenum e) { const GLubyte* v = glGetString(e); return v ? reinterpret_cast<const char*>(v) : "?"; };
    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    Debug::Info("OpenGLWindow") << "GL vendor: " << glStr(GL_VENDOR) << " | renderer: " << glStr(GL_RENDERER)
        << " | version: " << glStr(GL_VERSION) << " | GLSL: " << glStr(GL_SHADING_LANGUAGE_VERSION)
        << ((flags & GL_CONTEXT_FLAG_DEBUG_BIT) ? " | debug context" : "") << "\n";

    if (glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(GLDebugCallback, nullptr);
    }

    // Default off: the FPS limit has its own microsecond pacer in ClientWindow::renderLoop(), and the driver default (often
    // vsync on) would clamp to the monitor refresh rate whatever the target (e.g. 144Hz never reaching 165/240).
    // ClientWindow::startRenderThread() applies the saved RenderSettings value right after construction.
    setVSync(false);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);      // counter-clockwise = front face (default)

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}