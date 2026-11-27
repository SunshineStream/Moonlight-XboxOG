# Apply dependency patches to build-directory copies while preserving upstream checkouts.
include_guard(GLOBAL)

# source_dir and output_dir are roots; patch_file applies to the source-relative paths in ARGN.
# Reset disposable inputs on each call and publish only changed outputs to preserve timestamps.
function(sunshine_patch_sources source_dir output_dir patch_file)
    find_package(Git REQUIRED)
    set(work "${output_dir}/patch-work")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch_file}")
    foreach(source IN LISTS ARGN)
        configure_file("${source_dir}/${source}" "${work}/${source}" COPYONLY)
    endforeach()

    # An explicit work tree confines git apply to these copies, including when
    # the build directory is inside the Sunshine checkout. Normalize CRLF on
    # input and write LF so patching is independent of the user's Git settings.
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=input "--work-tree=${work}"
                apply --whitespace=error "${patch_file}"
            WORKING_DIRECTORY "${work}"
            RESULT_VARIABLE patch_result
            OUTPUT_VARIABLE patch_output
            ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR
                "Failed to apply ${patch_file}; review it against the sources in ${source_dir}.\n"
                "${patch_output}${patch_error}")
    endif()
    foreach(source IN LISTS ARGN)
        configure_file("${work}/${source}" "${output_dir}/${source}" COPYONLY)
    endforeach()
endfunction()
