cmake_minimum_required(VERSION 3.18)
get_filename_component(source_dir "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT GITHUB_CLONE_URL OR NOT GITHUB_COMMIT)
    message(FATAL_ERROR "Specify GITHUB_CLONE_URL and GITHUB_COMMIT.")
endif()

# Configure the application checkout and stage the checked-in dependency manifests.
set(_app_id dev.lizardbyte.app.Moonlight)
configure_file("${CMAKE_CURRENT_LIST_DIR}/${_app_id}.yml" "${_app_id}.yml" @ONLY)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/modules" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
set(_patch sdl3-steam-controller-trigger-clicks-and-haptics.patch)
configure_file("${source_dir}/cmake/patches/${_patch}" "modules/moonlight/${_patch}" COPYONLY)
