#include "OpenGLWindow.hpp"
#include <iostream>
#include <stdexcept>

// -------------------- Constructor / Destructor --------------------

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

    // Framebuffer resize callback
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int w, int h){
        glViewport(0, 0, w, h);
    });

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
}

OpenGLWindow::~OpenGLWindow() {
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
}

// -------------------- Window Operations --------------------

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

// Add to OpenGLWindow.hpp
void OpenGLWindow::releaseContext() {
    glfwMakeContextCurrent(nullptr);
}

void OpenGLWindow::close() { 
    glfwSetWindowShouldClose(window, GLFW_TRUE); 
}

// -------------------- Window Info --------------------

int OpenGLWindow::getWidth() const {
    int width;
    glfwGetFramebufferSize(window, &width, nullptr);
    return width;
}

int OpenGLWindow::getHeight() const {
    int height;
    glfwGetFramebufferSize(window, nullptr, &height);
    return height;
}

float OpenGLWindow::getAspectRatio() const {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    return h > 0 ? (float)w / (float)h : 1.0f;
}

// -------------------- OpenGL Setup --------------------

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

void OpenGLWindow::setupOpenGL()
{
    // Depth testing
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Backface culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);      // Cull back-facing triangles
    glFrontFace(GL_CCW);      // Counter-clockwise = front face (default)

    // Blending (transparency)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
