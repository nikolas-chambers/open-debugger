cmake_minimum_required(VERSION 3.20)
project(open_debugger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DBGENG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# The plugin SDK is its own repo (it ships to plugin authors), carried here as
# a submodule - the host has to compile against the exact same header it asks
# plugins to build against. Pulled in before the odbg target so it can link
# odbg::plugin_sdk; the plugin repos come in at the bottom, once odbg exists.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/open-debugger-plugins_sdk/CMakeLists.txt")
    message(FATAL_ERROR
        "The plugin SDK submodule is empty - odbg cannot compile without its header. Run:\n"
        "  git submodule update --init")
endif()
set(ODBG_SDK_BUILD_SAMPLE ON)  # in-tree, the SDK's sample is just another plugin
add_subdirectory(plugins/open-debugger-plugins_sdk)

# Everything in this project builds for either architecture from the same
# CMakeLists.txt - configure a build tree with `-A x64` or `-A Win32` and
# odbg_cli/odbg come out matching. third_party/{lib,bin} are laid out as x64/
# and x86/ subfolders to match. The import libs and headers are committed; the
# Debugging Tools DLLs under bin/ are Microsoft's and are fetched on demand
# instead (see cmake/FetchDbgEng.cmake), so both architectures get the same
# full set - symsrv/srcsrv/msdia140 included.
if(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
    set(ARCH_DIR "x86")
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
    set(ARCH_DIR "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_DIR "x64")
else()
    set(ARCH_DIR "x86")
endif()

set(DBGENG_LIB_DIR "${DBGENG_ROOT}/lib/${ARCH_DIR}")
set(DBGENG_BIN_DIR "${DBGENG_ROOT}/bin/${ARCH_DIR}")

# Populates DBGENG_BIN_DIR from nuget.org on a fresh clone. Has to run before
# the glob below, which is what gets copied next to the built exes.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchDbgEng)
odbg_provide_dbgeng("${ARCH_DIR}" "${DBGENG_BIN_DIR}")

file(GLOB DBGENG_DLLS "${DBGENG_BIN_DIR}/*.dll")

# --- odbg_cli: scriptable DbgEng front-end (see src/main.cpp) -------------

add_executable(odbg_cli src/main.cpp src/dbghost.cpp)

target_include_directories(odbg_cli PRIVATE
    "${DBGENG_ROOT}/include"
    src)

target_compile_definitions(odbg_cli PRIVATE
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE)

target_link_libraries(odbg_cli PRIVATE
    dbgeng
    dbghelp
    Ole32)

target_link_directories(odbg_cli PRIVATE "${DBGENG_LIB_DIR}")

if(MSVC)
    target_compile_options(odbg_cli PRIVATE /W3 /EHsc)
endif()

# Copy the engine dlls next to the built exe so it can find them at runtime.
add_custom_command(TARGET odbg_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DBGENG_DLLS} $<TARGET_FILE_DIR:odbg_cli>)

# --- odbg: OllyDbg-style front end (Win32 + DX11 + Dear ImGui) ------------

set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

add_executable(odbg
    src/gui_main.cpp
    src/dbghost.cpp
    src/cmddispatch.cpp
    src/dbgsession.cpp
    src/plugin_manager.cpp
    src/host_exports.cpp
    src/settings.cpp
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp")

target_include_directories(odbg PRIVATE
    "${DBGENG_ROOT}/include"
    "${IMGUI_ROOT}"
    "${IMGUI_ROOT}/backends"
    src)

# The plugin SDK header, from the submodule - same one plugins compile against.
target_link_libraries(odbg PRIVATE odbg::plugin_sdk)

# Lets plugin DLLs link directly against odbg.exe's exported Odbg_* API,
# the same way a real Olly plugin links against ollydbg.exe.
set_target_properties(odbg PROPERTIES ENABLE_EXPORTS ON)

target_compile_definitions(odbg PRIVATE
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE)

target_link_libraries(odbg PRIVATE
    dbgeng
    dbghelp
    Ole32
    Comdlg32
    d3d11
    dxgi
    d3dcompiler)

target_link_directories(odbg PRIVATE "${DBGENG_LIB_DIR}")

if(MSVC)
    target_compile_options(odbg PRIVATE /W3 /EHsc)
    target_link_options(odbg PRIVATE /MAP)
endif()

add_custom_command(TARGET odbg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DBGENG_DLLS} $<TARGET_FILE_DIR:odbg>)

# --- odbg plugins -----------------------------------------------------
#
# Plugins live in their own repos, carried here as submodules under plugins/.
# Each one is built through the SDK's add_odbg_plugin(), which links the odbg
# target directly (defined above) and drops the DLL in <exe dir>/plugins.
#
#   open-debugger-plugins_sdk      the SDK + odbg-sample_plugin (added above)
#   open-debugger-plugins          odbg-anti_anti
#   open-debugger-plugins-private  plugins for a project that is not public yet
#
# A missing one is skipped rather than fatal: open-debugger-plugins-private is
# a private repo, so for anyone without access it simply never checks out and
# the build carries on without it.

foreach(plugin_repo open-debugger-plugins open-debugger-plugins-private)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/${plugin_repo}/CMakeLists.txt")
        add_subdirectory("plugins/${plugin_repo}")
    else()
        message(STATUS "plugins/${plugin_repo} is not checked out - skipping it")
    endif()
endforeach()
