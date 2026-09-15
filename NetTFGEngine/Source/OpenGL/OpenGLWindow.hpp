#ifndef OPENGLWINDOW_HPP
#define OPENGLWINDOW_HPP

#include "OpenGLIncludes.hpp"
#include <string>

class OpenGLWindow {
public:
    OpenGLWindow(int width, int height, const std::string& title);
    ~OpenGLWindow();

    void swapBuffers();
    void pollEvents();
    bool shouldClose() const;
    void makeContextCurrent();
    void releaseContext();
    void close();

    int getWidth() const;          // framebuffer size (physical pixels) � use for glViewport
    int getHeight() const;
    int getLogicalWidth() const;   // window size (logical pixels) � use for UI hit-testing
    int getLogicalHeight() const;
    float getAspectRatio() const;
    GLFWwindow* getWindow() { return window; }



    bool wasResized();  // returns true once, then resets the flag

private:
    void initializeGLFW();
    void initializeGLEW();
    void setupOpenGL();

    GLFWwindow* window;

    int currentWidth;   // framebuffer size (physical pixels) � for glViewport
    int currentHeight;
    int logicalWidth;   // window size (logical pixels) � matches glfwGetCursorPos
    int logicalHeight;
    bool resized = false;
};

#endif // OPENGLWINDOW_HPP