#ifndef OPENGLWINDOW_HPP
#define OPENGLWINDOW_HPP

#include "OpenGLIncludes.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"
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

    void       setWindowMode(WindowMode mode);
    WindowMode getWindowMode() const;

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

    // Windowed position/size, remembered so leaving fullscreen restores it
    // instead of guessing; refreshed just before entering fullscreen.
    int windowedX = 0;
    int windowedY = 0;
    int windowedWidth = 0;
    int windowedHeight = 0;
};

#endif // OPENGLWINDOW_HPP