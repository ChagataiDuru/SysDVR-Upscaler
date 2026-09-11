find_package(doctest CONFIG REQUIRED)
add_executable(ns60_tests
  FrameQueueTests.cpp
  PlaybackClockTests.cpp
  ColorMathTests.cpp
  MetricsTests.cpp
  SysDvrBridgeProtocolTests.cpp
  UpscalingTests.cpp
  AppConfigTests.cpp)
target_link_libraries(ns60_tests PRIVATE ns60_core doctest::doctest)
target_compile_features(ns60_tests PRIVATE cxx_std_20)
ns60_enable_warnings(ns60_tests)
include(doctest)
doctest_discover_tests(ns60_tests)

if(WIN32 AND NS60_BUILD_APP AND NS60_ENABLE_GPU_TESTS)
  # The strict multi-planar NV12 import is deliberately unsupported on the
  # tested NVIDIA driver. Keep the known-good GPU-copy path under regression.
  foreach(sample_name IN ITEMS flat_color ui_text fast_motion)
    set(sample_path "${CMAKE_SOURCE_DIR}/samples/${sample_name}_720p60.mp4")
    if(NOT EXISTS "${sample_path}")
      continue()
    endif()
    if(sample_name STREQUAL "flat_color")
      set(test_name d3d11_vulkan_interop_copy_recorded_smoke)
    else()
      set(test_name "d3d11_vulkan_interop_copy_${sample_name}_smoke")
    endif()
    add_test(NAME ${test_name}
      COMMAND ${CMAKE_COMMAND}
        "-DNS60_APP=$<TARGET_FILE:NexusStream60>"
        "-DNS60_SAMPLE=${sample_path}"
        "-DNS60_INTEROP_PATH=interop-copy"
        "-DNS60_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/${test_name}"
        "-DNS60_COMPARE=${CMAKE_CURRENT_SOURCE_DIR}/ComparePng.ps1"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunD3D11VulkanInterop.cmake")
    set_tests_properties(${test_name} PROPERTIES
      SKIP_REGULAR_EXPRESSION "NS60_GPU_SKIP:"
      RUN_SERIAL TRUE
      TIMEOUT 300)
  endforeach()
endif()
