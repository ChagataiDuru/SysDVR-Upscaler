function(ns60_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
  else()
    # Vulkan structs are value-initialized as {sType}; the remaining fields are
    # zeroed by design, which -Wextra's missing-field-initializers flags everywhere.
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
  endif()
endfunction()

