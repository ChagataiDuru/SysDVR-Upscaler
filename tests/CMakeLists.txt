find_package(doctest CONFIG REQUIRED)
add_executable(ns60_tests
  FrameQueueTests.cpp
  PlaybackClockTests.cpp
  ColorMathTests.cpp
  MetricsTests.cpp
  UpscalingTests.cpp
  AppConfigTests.cpp)
target_link_libraries(ns60_tests PRIVATE ns60_core doctest::doctest)
target_compile_features(ns60_tests PRIVATE cxx_std_20)
ns60_enable_warnings(ns60_tests)
include(doctest)
doctest_discover_tests(ns60_tests)
