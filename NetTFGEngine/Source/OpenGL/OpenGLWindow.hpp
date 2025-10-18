#ifndef OPENGLWINDOW_HPP
#define OPENGLWINDOW_HPP

#include "OpenGLIncludes.hpp"
#include <string>

class OpenGLWindow {
public:
    OpenGLWindow(int width, int height, const std::string& title);
    ~OpenGLWindow();

    // Window operations
    void swapBuffers();
    void pollEvents();
    bool shouldClose() const;
    void makeContextCurrent();
    void close();

    // Window info
    int getWidth() const;
    int getHeight() const;
    float getAspectRatio() const;
    GLFWwindow* getWindow() { return window; }

private:
    void initializeGLFW();
    void initializeGLEW();
    void setupOpenGL();

    GLFWwindow* window;
};

#endif // OPENGLWINDOW_HPP