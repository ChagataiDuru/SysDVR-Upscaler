#include "platform/Window.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace ns60 {
namespace {
int glfwUsers{};

void glfwError(int, const char* description) {
    // Initialization failures are also surfaced by the checked API call.
    (void)description;
}

std::vector<GLFWmonitor*> monitors() {
    int count{};
    GLFWmonitor** raw = glfwGetMonitors(&count);
    if (!raw || count <= 0) return {};
    return {raw, raw + count};
}

GLFWmonitor* selectMonitor(std::optional<int> requested, int& selectedIndex) {
    const auto list = monitors();
    if (list.empty()) throw std::runtime_error("GLFW did not report any monitors");
    if (requested) {
        if (*requested < 0 || *requested >= static_cast<int>(list.size())) {
            throw std::runtime_error("Invalid --monitor index " + std::to_string(*requested) +
                "; available monitor indexes are 0 through " + std::to_string(static_cast<int>(list.size()) - 1));
        }
        selectedIndex = *requested;
        return list[static_cast<std::size_t>(*requested)];
    }
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const auto found = std::find(list.begin(), list.end(), primary);
    selectedIndex = found != list.end() ? static_cast<int>(std::distance(list.begin(), found)) : 0;
    return list[static_cast<std::size_t>(selectedIndex)];
}
} // namespace

Window::Window(int width, int height, std::string title, bool fullscreen, bool borderless, std::optional<int> monitorIndex)
    : fullscreen_(fullscreen), borderless_(borderless), windowedWidth_(width), windowedHeight_(height) {
    if (glfwUsers++ == 0) {
        glfwSetErrorCallback(glfwError);
        if (glfwInit() != GLFW_TRUE) {
            --glfwUsers;
            throw std::runtime_error("GLFW initialization failed; verify that an interactive Windows desktop is available");
        }
    }
    monitor_ = selectMonitor(monitorIndex, monitorIndex_);
    const GLFWvidmode* mode = glfwGetVideoMode(monitor_);
    if (!mode) throw std::runtime_error("Selected monitor does not expose a video mode");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, borderless ? GLFW_FALSE : GLFW_TRUE);

    int initialWidth = width;
    int initialHeight = height;
    GLFWmonitor* exclusiveMonitor = nullptr;
    if (fullscreen) {
        initialWidth = mode->width;
        initialHeight = mode->height;
        exclusiveMonitor = borderless ? nullptr : monitor_;
    }
    window_ = glfwCreateWindow(initialWidth, initialHeight, title.c_str(), exclusiveMonitor, nullptr);
    if (!window_) {
        if (--glfwUsers == 0) glfwTerminate();
        throw std::runtime_error("Failed to create the GLFW Vulkan window");
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, keyCallback);
    glfwSetScrollCallback(window_, scrollCallback);

    if (fullscreen && borderless) {
        int monitorX{}, monitorY{};
        glfwGetMonitorPos(monitor_, &monitorX, &monitorY);
        glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowPos(window_, monitorX, monitorY);
        glfwSetWindowSize(window_, mode->width, mode->height);
    } else if (!fullscreen) {
        adjustWindowForFramebuffer(width, height);
    }
}

Window::~Window() {
    if (window_) glfwDestroyWindow(window_);
    if (--glfwUsers == 0) glfwTerminate();
}

bool Window::shouldClose() const noexcept { return glfwWindowShouldClose(window_) == GLFW_TRUE; }
void Window::requestClose() noexcept { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
void Window::pollEvents() noexcept { glfwPollEvents(); }
void Window::waitEvents(double timeoutSeconds) noexcept { glfwWaitEventsTimeout(timeoutSeconds); }

WindowEvents Window::consumeEvents() noexcept {
    const auto result = events_;
    events_ = {};
    return result;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface{};
    const VkResult result = glfwCreateWindowSurface(instance, window_, nullptr, &surface);
    if (result != VK_SUCCESS) throw std::runtime_error("GLFW failed to create a Vulkan Win32 surface (VkResult " + std::to_string(result) + ")");
    return surface;
}

bool Window::framebufferExtent(int& width, int& height) const noexcept {
    glfwGetFramebufferSize(window_, &width, &height);
    return width > 0 && height > 0;
}

WindowSizeInfo Window::sizeInfo() const noexcept {
    WindowSizeInfo info{};
    glfwGetWindowSize(window_, &info.windowWidth, &info.windowHeight);
    glfwGetFramebufferSize(window_, &info.framebufferWidth, &info.framebufferHeight);
    glfwGetWindowContentScale(window_, &info.contentScaleX, &info.contentScaleY);
    return info;
}

MonitorInfo Window::monitorInfo() const noexcept {
    MonitorInfo info{};
    info.index = monitorIndex_;
    if (monitor_) {
        if (const char* name = glfwGetMonitorName(monitor_)) info.name = name;
        glfwGetMonitorPos(monitor_, &info.x, &info.y);
        glfwGetMonitorContentScale(monitor_, &info.contentScaleX, &info.contentScaleY);
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor_)) {
            info.width = mode->width;
            info.height = mode->height;
            info.refreshRate = mode->refreshRate;
        }
    }
    return info;
}

void Window::adjustWindowForFramebuffer(int framebufferWidth, int framebufferHeight) noexcept {
    int actualWidth{}, actualHeight{};
    glfwGetFramebufferSize(window_, &actualWidth, &actualHeight);
    if (actualWidth == framebufferWidth && actualHeight == framebufferHeight) return;

    float scaleX{1.0F}, scaleY{1.0F};
    glfwGetWindowContentScale(window_, &scaleX, &scaleY);
    if (scaleX <= 0.0F || scaleY <= 0.0F) return;
    const int clientWidth = std::max(1, static_cast<int>(std::lround(static_cast<float>(framebufferWidth) / scaleX)));
    const int clientHeight = std::max(1, static_cast<int>(std::lround(static_cast<float>(framebufferHeight) / scaleY)));
    glfwSetWindowSize(window_, clientWidth, clientHeight);
    glfwPollEvents();
}

void Window::toggleFullscreen() {
    const GLFWvidmode* mode = monitor_ ? glfwGetVideoMode(monitor_) : nullptr;
    if (!mode) return;
    if (!fullscreen_) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);
        int monitorX{}, monitorY{};
        glfwGetMonitorPos(monitor_, &monitorX, &monitorY);
        glfwSetWindowAttrib(window_, GLFW_DECORATED, borderless_ ? GLFW_FALSE : GLFW_TRUE);
        glfwSetWindowMonitor(window_, borderless_ ? nullptr : monitor_, monitorX, monitorY, mode->width, mode->height,
                             borderless_ ? GLFW_DONT_CARE : mode->refreshRate);
        if (borderless_) glfwSetWindowPos(window_, monitorX, monitorY);
        fullscreen_ = true;
    } else {
        glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedWidth_, windowedHeight_, GLFW_DONT_CARE);
        fullscreen_ = false;
    }
}

void Window::keyCallback(GLFWwindow* window, int key, int, int action, int modifiers) noexcept {
    if (auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window))) self->onKey(key, action, modifiers);
}

void Window::scrollCallback(GLFWwindow* window, double, double yOffset) noexcept {
    if (auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window))) self->events_.zoomDelta += static_cast<float>(yOffset);
}

void Window::onKey(int key, int action, int modifiers) noexcept {
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_SPACE: events_.togglePause = true; break;
    case GLFW_KEY_RIGHT: events_.stepFrame = true; events_.zoomMoveX = 1; break;
    case GLFW_KEY_LEFT: events_.zoomMoveX = -1; break;
    case GLFW_KEY_UP: events_.zoomMoveY = -1; break;
    case GLFW_KEY_DOWN: events_.zoomMoveY = 1; break;
    case GLFW_KEY_HOME: events_.seekBeginning = true; break;
    case GLFW_KEY_F11: events_.toggleFullscreen = true; break;
    case GLFW_KEY_TAB: events_.toggleTelemetry = true; break;
    case GLFW_KEY_S: events_.screenshot = true; break;
    case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3: case GLFW_KEY_4:
    case GLFW_KEY_5: case GLFW_KEY_6: case GLFW_KEY_7: case GLFW_KEY_8: events_.upscaleMode = key - GLFW_KEY_1; break;
    case GLFW_KEY_C: events_.toggleComparison = true; break;
    case GLFW_KEY_A: events_.setComparisonA = true; break;
    case GLFW_KEY_B: events_.setComparisonB = true; break;
    case GLFW_KEY_Z: events_.toggleZoom = true; break;
    case GLFW_KEY_LEFT_BRACKET: events_.sharpnessDelta = (modifiers & GLFW_MOD_SHIFT) ? -0.10F : -0.02F; break;
    case GLFW_KEY_RIGHT_BRACKET: events_.sharpnessDelta = (modifiers & GLFW_MOD_SHIFT) ? 0.10F : 0.02F; break;
    case GLFW_KEY_ESCAPE: events_.escape = true; break;
    default: break;
    }
}

} // namespace ns60