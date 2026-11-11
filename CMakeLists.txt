cmake_minimum_required(VERSION 3.20)

project(onetbb-windows-dllmain-crash LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "This reproducer targets Windows. On Linux, use scripts/build-wine.sh "
        "to cross-build it with MSVC under Wine.")
endif()

# Keep the reproducer self-contained: a static CRT means the staged directory
# can be copied to any Windows machine without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# oneTBB revision under test
#
# The default is upstream master before the deferred-unload change:
#   f1e7e5cbf0b77a94d27700141b9188580bd9c778
# Other interesting revisions:
#   5ec58d697b319b428c3a2cde7020d9f5150492b6  (first commit that unloads from DllMain)
#   30337af8f8aee91e03beec6da27bf7216e50968e  (guard: no FreeLibrary from DllMain)
# ---------------------------------------------------------------------------
set(ONETBB_GIT_REPOSITORY "https://github.com/uxlfoundation/oneTBB.git" CACHE STRING
    "Git repository to fetch oneTBB from")
set(ONETBB_GIT_TAG "f1e7e5cbf0b77a94d27700141b9188580bd9c778" CACHE STRING
    "oneTBB commit to build against")

set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TBB_STRICT OFF CACHE BOOL "" FORCE)
set(TBB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_PROXY_BUILD OFF CACHE BOOL "" FORCE)
set(TBB_VERIFY_DEPENDENCY_SIGNATURE OFF CACHE BOOL "" FORCE)
# TCM needs hwloc, which the reproducer does not use. Leave it out so that a
# fresh configure works on a machine without hwloc.
set(TCM_BUILD OFF CACHE BOOL "" FORCE)
set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(onetbb
    GIT_REPOSITORY "${ONETBB_GIT_REPOSITORY}"
    GIT_TAG "${ONETBB_GIT_TAG}"
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(onetbb)

# ---------------------------------------------------------------------------
# Reproducer binaries
# ---------------------------------------------------------------------------

# Fake tbbmalloc.dll. oneTBB resolves this by name next to tbb12.dll.
add_library(fake_tbbmalloc SHARED src/fake_tbbmalloc.cpp)
set_target_properties(fake_tbbmalloc PROPERTIES OUTPUT_NAME tbbmalloc)

# Module that the host unloads while oneTBB still runs.
add_library(callback_module SHARED src/callback.cpp)
set_target_properties(callback_module PROPERTIES OUTPUT_NAME callback)

# Plugin that links oneTBB.
add_library(plugin SHARED src/plugin.cpp)
set_target_properties(plugin PROPERTIES OUTPUT_NAME plugin)
target_link_libraries(plugin PRIVATE TBB::tbb)
target_compile_features(plugin PRIVATE cxx_std_17)

# Host that does not link oneTBB.
add_executable(host src/host.cpp)
target_compile_features(host PRIVATE cxx_std_17)
target_compile_definitions(host PRIVATE
    ONETBB_DLL_NAME="$<TARGET_FILE_NAME:tbb>"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
# Staging directory: everything the crash needs, in one folder
# ---------------------------------------------------------------------------
set(DIST_DIR "${CMAKE_BINARY_DIR}/dist")

add_custom_target(dist ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIST_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:host>" "${DIST_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:plugin>" "${DIST_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:callback_module>" "${DIST_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:fake_tbbmalloc>" "${DIST_DIR}/tbbmalloc.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:fake_tbbmalloc>" "${DIST_DIR}/tbbmalloc_debug.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:tbb>" "${DIST_DIR}/"
    COMMENT "Staging the reproducer in ${DIST_DIR}"
    VERBATIM)

add_dependencies(dist host plugin callback_module fake_tbbmalloc tbb)
