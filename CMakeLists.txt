cmake_minimum_required(VERSION 3.31...4.4.2)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "" FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)

project(
    capusy
    VERSION 0.1.0
    LANGUAGES C CXX RC
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

include(CMakeDependentOption)
include(FetchContent)

##### Options
option(CAPUSY_USE_MIMALLOC "Replace default allocator with mimalloc" ON)
option(CAPUSY_USE_INTERPROCEDURAL_OPTIMIZATION "Enable FLTO/LTCG" ON)
cmake_dependent_option(CAPUSY_USE_STATIC_MSVCRT
    "Use the static release/debug MSVCRT - or set the flag yourself before including the project" ON
    "MSVC" OFF
)

macro(set_tracked_cache_version VAR_NAME DEFAULT_VAL DOCSTRING)
    if(NOT "${${VAR_NAME}_LAST_DEFAULT}" STREQUAL "${DEFAULT_VAL}")
        set(${VAR_NAME} "${DEFAULT_VAL}" CACHE STRING "${DOCSTRING}" FORCE)
        set(${VAR_NAME}_LAST_DEFAULT "${DEFAULT_VAL}" CACHE INTERNAL "Tracks default change for ${VAR_NAME}")
    else()
        set(${VAR_NAME} "${DEFAULT_VAL}" CACHE STRING "${DOCSTRING}")
    endif()
endmacro()

set_tracked_cache_version(CAPUSY_USYLIBPP_VERSION "main" "usylibpp git tag/branch")
set_tracked_cache_version(CAPUSY_GLAZE_VERSION "v9.1.0" "glaze git tag")
set_tracked_cache_version(CAPUSY_MIMALLOC_VERSION "v3.7.1" "Version (git tag/branch) to use for mimalloc")
set_tracked_cache_version(CAPUSY_SQLITE_VERSION "3540100" "SQLite amalgamation release")

# Status Reports
if(WIN32)
    message(STATUS "[${PROJECT_NAME}] Using static MSVCRT: ${CAPUSY_USE_STATIC_MSVCRT} (CAPUSY_USE_STATIC_MSVCRT)")
endif()
message(STATUS "[${PROJECT_NAME}] Using interprocedural optimization: ${CAPUSY_USE_INTERPROCEDURAL_OPTIMIZATION} (CAPUSY_USE_INTERPROCEDURAL_OPTIMIZATION)")
message(STATUS "[${PROJECT_NAME}] Using mimalloc: ${CAPUSY_USE_MIMALLOC} (CAPUSY_USE_MIMALLOC)")
if(CAPUSY_USE_MIMALLOC)
    message(STATUS "[${PROJECT_NAME}] Using mimalloc version: ${CAPUSY_MIMALLOC_VERSION} (CAPUSY_MIMALLOC_VERSION)")
endif()

if(CAPUSY_USE_STATIC_MSVCRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(CAPUSY_USE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

set(CAPUSY_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CAPUSY_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CAPUSY_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CAPUSY_BIN_DIR}")

##### FetchContent dependencies
set(USYLIBPP_ENABLE_WINDOWS ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINDOWS_UNICODE ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WIL ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINDOWS_IMAGING ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_TASK_DIALOG_DARK_MODE ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_HTTP ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_UPDATER ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_UI ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINTOAST OFF CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_VOIDTOOLS_EVERYTHING OFF CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_VOIDTOOLS_EVERYTHING3 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    usylibpp
    GIT_REPOSITORY https://github.com/usyless/usylibpp.git
    GIT_TAG "${CAPUSY_USYLIBPP_VERSION}"
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG "${CAPUSY_GLAZE_VERSION}"
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(
    sqlite3
    URL "https://sqlite.org/2026/sqlite-amalgamation-${CAPUSY_SQLITE_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(usylibpp glaze sqlite3)

add_library(capusy_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
set_target_properties(capusy_sqlite3 PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(capusy_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(capusy_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(capusy_sqlite3 PRIVATE /W0)
endif()

##### Common compile settings
foreach(_part IN ITEMS MAJOR MINOR PATCH TWEAK)
    if(DEFINED PROJECT_VERSION_${_part} AND NOT "${PROJECT_VERSION_${_part}}" STREQUAL "")
        set(CAPUSY_VERSION_${_part} "${PROJECT_VERSION_${_part}}")
    else()
        set(CAPUSY_VERSION_${_part} 0)
    endif()
endforeach()
set(CAPUSY_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file("${PROJECT_SOURCE_DIR}/resources/version.h.in" "${CAPUSY_GENERATED_DIR}/capusy_version.h" @ONLY)

add_library(capusy_common INTERFACE)
target_include_directories(capusy_common INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/resources"
    "${CAPUSY_GENERATED_DIR}"
)
target_include_directories(capusy_common SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/3rdparty")
target_link_libraries(capusy_common INTERFACE usylibpp::usylibpp glaze::glaze version)
target_compile_definitions(capusy_common INTERFACE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Release>:NDEBUG>
)
if(MSVC)
    target_compile_options(capusy_common INTERFACE
        /EHsc
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /bigobj
        $<$<CONFIG:Debug>:/Zi /Od>
        $<$<CONFIG:Release>:/O2 /Ob2 /Oi /Gy /Gw>
    )
    target_link_options(capusy_common INTERFACE
        $<$<CONFIG:Debug>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
endif()

# mimalloc Configuration (static CRT override is only supported in non-Debug builds on MSVC)
if(CAPUSY_USE_MIMALLOC AND NOT (MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug"))
    target_compile_definitions(capusy_common INTERFACE
        CAPUSY_USE_MIMALLOC
    )

    if(WIN32)
        if(NOT CMAKE_MSVC_RUNTIME_LIBRARY OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
            message(WARNING "[${PROJECT_NAME}] mimalloc static overloading on Windows requires the static CRT runtime (/MT). Currently CMAKE_MSVC_RUNTIME_LIBRARY is '${CMAKE_MSVC_RUNTIME_LIBRARY}'.")
        endif()
    endif()

    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG "${CAPUSY_MIMALLOC_VERSION}"
        EXCLUDE_FROM_ALL
        SYSTEM
    )
    FetchContent_MakeAvailable(mimalloc)

    target_link_libraries(capusy_common INTERFACE
        mimalloc-obj
    )
    target_sources(capusy_common INTERFACE
        $<TARGET_OBJECTS:mimalloc-obj>
    )
endif()

##### capusy.exe
add_executable(capusy WIN32
    src/main.cpp
    resources/app.rc
    resources/app.manifest
)
set_target_properties(capusy PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(capusy PRIVATE
    capusy_common
    capusy_sqlite3
    d2d1 dwrite d3d11 dxgi dxguid dcomp dwmapi windowscodecs
    mfplat mfreadwrite mfuuid mf
    runtimeobject
    shell32 shlwapi shcore ole32 oleaut32 user32 gdi32 imm32 uxtheme comdlg32 propsys winhttp bcrypt
)
