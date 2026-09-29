#ifndef OPENGLWINDOW_HPP
#define OPENGLWINDOW_HPP

#include "OpenGLIncludes.hpp"
#include "OpenGL/Render pipeline/RenderSettings.hpp"
#include <string>
#include <vector>
#include <utility>

// The window is never resizable by the user: its size only comes from the settings. Windowed uses the chosen
// window resolution; Borderless and Fullscreen always cover the monitor at its native resolution.
class OpenGLWindow {
public:
    OpenGLWindow(int width, int height, const std::string& title);
    ~OpenGLWindow();

    void swapBuffers();
    void pollEvents();
    bool shouldClose() const;
    // True while iconified (also what Alt+Tab from exclusive fullscreen does);
    // the framebuffer is 0x0 then, so callers should skip rendering.
    bool isMinimized() const;
    void makeContextCurrent();
    void releaseContext();
    void close();

    void       setWindowMode(WindowMode mode);
    WindowMode getWindowMode() const;

    // Size used in Windowed mode (clamped to the monitor). Applied right away, re-centred, when already windowed;
    // otherwise remembered for the next switch back to Windowed.
    void setWindowedSize(int width, int height);

    // Native resolution of the primary monitor: the fixed size of Borderless / Fullscreen.
    static void getMonitorResolution(int& width, int& height);
    // Distinct sizes the primary monitor supports (ascending, tiny legacy modes dropped); the windowed choices.
    static std::vector<std::pair<int, int>> getAvailableResolutions();

    // glfwSwapInterval(1)/(0). Reasserted after setWindowMode() too, since
    // glfwSetWindowMonitor can recreate the swap chain (e.g. toggling
    // exclusive fullscreen) and silently reset the driver's interval.
    void setVSync(bool enabled);
    bool getVSync() const { return vsyncEnabled; }

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
    void centerWindowedPosition();

    GLFWwindow* window;

    int currentWidth;   // framebuffer size (physical pixels) � for glViewport
    int currentHeight;
    int logicalWidth;   // window size (logical pixels) � matches glfwGetCursorPos
    int logicalHeight;
    bool resized = false;

    // Windowed position/size. The size is the window resolution setting; the position is remembered so leaving
    // fullscreen puts the window back where it was (refreshed just before leaving Windowed).
    int windowedX = 0;
    int windowedY = 0;
    int windowedWidth = 0;
    int windowedHeight = 0;

    bool vsyncEnabled = false;
};

#endif // OPENGLWINDOW_HPP