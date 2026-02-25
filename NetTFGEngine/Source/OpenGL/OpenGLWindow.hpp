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
    void releaseContext();
    void close();

    // Window info
    int getWidth() const;
    int getHeight() const;
    float getAspectRatio() const;
    GLFWwindow* getWindow() { return window; }

    

    // Public
    bool wasResized();  // returns true once, then resets the flag

private:
    void initializeGLFW();
    void initializeGLEW();
    void setupOpenGL();

    GLFWwindow* window;

    // Private members
    int currentWidth;
    int currentHeight;
    bool resized = false;
};

#endif // OPENGLWINDOW_HPP