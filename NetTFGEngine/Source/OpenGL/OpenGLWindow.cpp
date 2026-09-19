#include "OpenGLWindow.hpp"
#include <iostream>
#include <stdexcept>

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
        glViewport(0, 0, w, h);
        auto* self = static_cast<OpenGLWindow*>(glfwGetWindowUserPointer(win));
        self->currentWidth = w;
        self->currentHeight = h;
        self->resized = true;
        });

    // Window size callback � logical pixels, matches glfwGetCursorPos space
    glfwSetWindowSizeCallback(window, [](GLFWwindow* win, int w, int h) {
        auto* self = static_cast<OpenGLWindow*>(glfwGetWindowUserPointer(win));
        self->logicalWidth = w;
        self->logicalHeight = h;
        });

    glfwGetFramebufferSize(window, &currentWidth, &currentHeight);
    glViewport(0, 0, currentWidth, currentHeight);

    glfwGetWindowSize(window, &logicalWidth, &logicalHeight);

    glfwGetWindowPos(window, &windowedX, &windowedY);
    windowedWidth = logicalWidth;
    windowedHeight = logicalHeight;
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

    // Remember the windowed geometry before leaving it, so Windowed can be
    // restored exactly regardless of how many times Borderless/Fullscreen
    // were toggled in between.
    if (getWindowMode() == WindowMode::Windowed) {
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedWidth, &windowedHeight);
    }

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

    // glfwSetWindowMonitor recreates the swap chain on some drivers
    // (notably switching into/out of exclusive fullscreen), which can
    // silently reset the swap interval back to the driver default. Re-assert
    // whatever VSync was configured to so it doesn't flip back on/off.
    glfwSwapInterval(vsyncEnabled ? 1 : 0);
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

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
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
    // Default off: the FPS-limit setting has its own microsecond-precision
    // pacer in ClientWindow::renderLoop(); leaving the driver's default
    // swap interval (often 1 = vsync on) fights it and silently clamps
    // actual frame rate to the monitor's refresh rate no matter what target
    // FPS is picked (e.g. a 144Hz panel never showing 165/240 even though
    // the pacer asks for it). ClientWindow::startRenderThread() applies the
    // saved RenderSettings value right after construction.
    setVSync(false);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);      // counter-clockwise = front face (default)

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}