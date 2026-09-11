if(NOT DEFINED NS60_APP OR NOT EXISTS "${NS60_APP}")
  message(FATAL_ERROR "NexusStream60 executable was not provided")
endif()
if(NOT DEFINED NS60_SAMPLE OR NOT EXISTS "${NS60_SAMPLE}")
  message(FATAL_ERROR "Recorded H.264 integration sample was not provided")
endif()
if(NOT DEFINED NS60_WORK_DIR OR NOT DEFINED NS60_COMPARE OR NOT EXISTS "${NS60_COMPARE}")
  message(FATAL_ERROR "Work directory and PNG comparison script must be provided")
endif()
if(NOT DEFINED NS60_CAPTURE_FRAME)
  set(NS60_CAPTURE_FRAME 60)
endif()
if(NOT DEFINED NS60_INTEROP_PATH)
  set(NS60_INTEROP_PATH interop)
endif()

execute_process(
  COMMAND "${NS60_APP}" --decoder-capabilities
  RESULT_VARIABLE capability_result
  OUTPUT_VARIABLE capability_output
  ERROR_VARIABLE capability_error)
if(NOT capability_result EQUAL 0)
  message(FATAL_ERROR
    "Decoder capability audit failed (${capability_result})\n${capability_output}\n${capability_error}")
endif()

set(required_capabilities
  "D3D11VA FFmpeg device: available"
  "D3D11 texture import: rgba8=yes, nv12=yes"
  "Interop features: timeline semaphore=yes, timeline D3D11_FENCE import=yes"
  "D3D11/Vulkan same GPU: yes")
# Only the zero-copy path imports the multi-slice decoder array as a layered image.
if(NS60_INTEROP_PATH STREQUAL "interop")
  list(APPEND required_capabilities "Y'CbCr image arrays (layered NV12 import): extension=yes, feature=yes")
endif()
foreach(required IN LISTS required_capabilities)
  string(FIND "${capability_output}" "${required}" found_at)
  if(found_at EQUAL -1)
    message("NS60_GPU_SKIP: required capability is unavailable: ${required}")
    return()
  endif()
endforeach()

# Each decoder path runs in its own working directory so its captures/ output
# can be located without parsing screenshot file names.
function(ns60_run_capture path out_png)
  set(work "${NS60_WORK_DIR}/${path}")
  file(REMOVE_RECURSE "${work}")
  file(MAKE_DIRECTORY "${work}")
  execute_process(
    COMMAND "${NS60_APP}"
      --input "${NS60_SAMPLE}"
      --decoder d3d11va
      --decoder-path ${path}
      --capture-frame ${NS60_CAPTURE_FRAME}
      --validation on
      --vsync off
      --log-level info
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
      "D3D11VA ${path} recorded-file run failed (${run_result})\n${run_output}\n${run_error}")
  endif()
  # Vulkan validation errors are routed to Log::error by the debug messenger.
  string(FIND "${run_output}${run_error}" "] [ERROR] " error_at)
  string(FIND "${run_output}${run_error}" "] [CRITICAL] " critical_at)
  if(NOT error_at EQUAL -1 OR NOT critical_at EQUAL -1)
    message(FATAL_ERROR "D3D11VA ${path} run logged errors (validation?)\n${run_output}\n${run_error}")
  endif()
  if(NOT path STREQUAL "readback")
    string(FIND "${run_output}${run_error}" "Actual decoder texture import: success" import_at)
    if(import_at EQUAL -1)
      message(FATAL_ERROR
        "Interop process completed without proving a real decoder texture import\n${run_output}\n${run_error}")
    endif()
  endif()
  file(GLOB captures "${work}/captures/*.png")
  list(LENGTH captures capture_count)
  if(NOT capture_count EQUAL 1)
    message(FATAL_ERROR "D3D11VA ${path} run produced ${capture_count} captures; expected exactly one")
  endif()
  set(${out_png} "${captures}" PARENT_SCOPE)
endfunction()

ns60_run_capture(readback readback_png)
ns60_run_capture(${NS60_INTEROP_PATH} interop_png)

execute_process(
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${NS60_COMPARE}"
    -Reference "${readback_png}" -Candidate "${interop_png}" -Tolerance 1
  RESULT_VARIABLE compare_result
  OUTPUT_VARIABLE compare_output
  ERROR_VARIABLE compare_error)
message("${compare_output}")
if(NOT compare_result EQUAL 0)
  message(FATAL_ERROR
    "Interop output differs from readback output beyond tolerance\n${compare_output}\n${compare_error}")
endif()
