#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <optional>
#include <string>

namespace ns60 {

struct WindowEvents {
    bool togglePause{};
    bool stepFrame{};
    bool seekBeginning{};
    bool toggleFullscreen{};
    bool toggleTelemetry{};
    bool screenshot{};
    int upscaleMode{-1};
    bool toggleComparison{};
    bool setComparisonA{};
    bool setComparisonB{};
    bool toggleZoom{};
    float sharpnessDelta{};
    float zoomDelta{};
    int zoomMoveX{}, zoomMoveY{};
    bool escape{};
};

struct WindowSizeInfo {
    int windowWidth{};
    int windowHeight{};
    int framebufferWidth{};
    int framebufferHeight{};
    float contentScaleX{1.0F};
    float contentScaleY{1.0F};
};

struct MonitorInfo {
    int index{};
    std::string name;
    int x{};
    int y{};
    int width{};
    int height{};
    int refreshRate{};
    float contentScaleX{1.0F};
    float contentScaleY{1.0F};
};

class Window final {
public:
    Window(int width, int height, std::string title, bool fullscreen, bool borderless, std::optional<int> monitorIndex = std::nullopt);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] GLFWwindow* native() const noexcept { return window_; }
    [[nodiscard]] bool shouldClose() const noexcept;
    void requestClose() noexcept;
    void pollEvents() noexcept;
    void waitEvents(double timeoutSeconds) noexcept;
    [[nodiscard]] WindowEvents consumeEvents() noexcept;
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;
    [[nodiscard]] bool framebufferExtent(int& width, int& height) const noexcept;
    [[nodiscard]] WindowSizeInfo sizeInfo() const noexcept;
    [[nodiscard]] MonitorInfo monitorInfo() const noexcept;
    void toggleFullscreen();
    [[nodiscard]] bool fullscreen() const noexcept { return fullscreen_; }

private:
    static void keyCallback(GLFWwindow* window, int key, int scanCode, int action, int modifiers) noexcept;
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset) noexcept;
    void onKey(int key, int action, int modifiers) noexcept;
    void adjustWindowForFramebuffer(int framebufferWidth, int framebufferHeight) noexcept;

    GLFWwindow* window_{};
    GLFWmonitor* monitor_{};
    WindowEvents events_{};
    bool fullscreen_{};
    bool borderless_{};
    int monitorIndex_{};
    int windowedX_{100};
    int windowedY_{100};
    int windowedWidth_{1280};
    int windowedHeight_{720};
};

} // namespace ns60