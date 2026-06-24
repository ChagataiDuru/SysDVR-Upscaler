cmake_minimum_required(VERSION 3.24)

option(NS60_BUILD_APP "Build the Vulkan playback application" ON)
option(NS60_BUILD_TESTS "Build unit tests" ON)
project(NexusStream60 VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(ns60_validation_default ON)
else()
  set(ns60_validation_default OFF)
endif()
option(NS60_ENABLE_VALIDATION "Enable Vulkan validation by default" ${ns60_validation_default})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_library(ns60_core
  src/app/AppConfig.cpp
  src/decode/FramePool.cpp
  src/decode/FrameQueue.cpp
  src/playback/PlaybackClock.cpp
  src/render/Upscaling.cpp
  src/telemetry/Metrics.cpp
  src/utility/ColorMath.cpp
  src/utility/Log.cpp)
target_include_directories(ns60_core PUBLIC src)
target_compile_features(ns60_core PUBLIC cxx_std_20)
ns60_enable_warnings(ns60_core)

if(NS60_BUILD_APP)
  find_package(Vulkan 1.2 REQUIRED)
  find_package(glfw3 CONFIG REQUIRED)
  find_package(imgui CONFIG REQUIRED)
  find_path(Stb_INCLUDE_DIR stb_image_write.h REQUIRED)
  find_package(FFmpeg REQUIRED)

  add_executable(NexusStream60
    src/main.cpp
    src/app/Application.cpp
    src/decode/FFmpegRuntime.cpp
    src/decode/FFmpegVideoReader.cpp
    src/platform/Window.cpp
    src/render/FidelityFxConstants.cpp
    src/render/VulkanContext.cpp
    src/render/VideoPipeline.cpp
    src/telemetry/ImGuiOverlay.cpp)
  target_link_libraries(NexusStream60 PRIVATE ns60_core Vulkan::Vulkan glfw imgui::imgui
    FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
  target_include_directories(NexusStream60 SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
  target_include_directories(NexusStream60 SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1")
  target_compile_definitions(NexusStream60 PRIVATE
    NS60_VERSION="${PROJECT_VERSION}"
    NS60_DEFAULT_VALIDATION=$<IF:$<BOOL:${NS60_ENABLE_VALIDATION}>,1,0>
    NOMINMAX WIN32_LEAN_AND_MEAN)
  ns60_enable_warnings(NexusStream60)
  include(CompileShaders)
  ns60_compile_shaders(NexusStream60
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/yuv420p_to_rgb.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bilinear_upscale.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_nearest.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_bicubic_catmull_rom.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale_lanczos2.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharpen_cas.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr1_easu.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr1_rcas.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen_present.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen_present.frag")
endif()

if(NS60_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

