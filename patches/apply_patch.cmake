# Apply a unified diff to a private copy of an upstream source file.
#
# The copy lives in the build tree, so lib/CMSIS-DAP stays untouched. The patch is applied to a
# temporary copy whose line endings match the source, so it works whether the upstream file was
# checked out with LF or CRLF.

foreach(var SOURCE PATCH OUTPUT WORKDIR GIT_EXECUTABLE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "apply_patch.cmake: ${var} is not defined")
    endif()
endforeach()

file(READ "${SOURCE}" content)
string(REPLACE "\r\n" "\n" content "${content}")

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}" "${content}")

# Write the patch through the same text path, so CMake applies the same newline conversion
file(READ "${PATCH}" patch_content)
string(REPLACE "\r\n" "\n" patch_content "${patch_content}")
set(local_patch "${WORKDIR}/apply_patch.patch")
file(WRITE "${local_patch}" "${patch_content}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths --whitespace=nowarn "${local_patch}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "Failed to apply ${PATCH}:\n${error_output}")
endif()
