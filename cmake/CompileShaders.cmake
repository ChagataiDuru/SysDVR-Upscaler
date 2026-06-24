function(ns60_compile_shaders target)
  find_program(NS60_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
  find_program(NS60_GLSLANG glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
  if(NOT NS60_GLSLC AND NOT NS60_GLSLANG)
    message(FATAL_ERROR
      "No GLSL-to-SPIR-V compiler found. Install the Vulkan SDK and set VULKAN_SDK; "
      "NexusStream60 accepts glslc or glslangValidator.")
  endif()

  set(outputs)
  foreach(shader IN LISTS ARGN)
    get_filename_component(name "${shader}" NAME)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
    if(NS60_GLSLC)
      add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${NS60_GLSLC}" --target-env=vulkan1.2 -O "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1"
          "${shader}" -o "${output}"
        DEPENDS "${shader}" VERBATIM)
    else()
      add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${NS60_GLSLANG}" -V --target-env vulkan1.2 "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1"
          "${shader}" -o "${output}"
        DEPENDS "${shader}" VERBATIM)
    endif()
    list(APPEND outputs "${output}")
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${outputs})
  add_dependencies(${target} ${target}_shaders)
  target_compile_definitions(${target} PRIVATE NS60_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
endfunction()

