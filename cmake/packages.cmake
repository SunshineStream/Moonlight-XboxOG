# Share Sunshine/build-deps' checked-in CPM and annotated package-lock convention.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/cpm/CPM.cmake")
CPMUsePackageLock("${CMAKE_SOURCE_DIR}/package-lock.cmake")
