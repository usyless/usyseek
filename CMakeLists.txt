cmake_minimum_required(VERSION 3.31...4.4.2)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "" FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)

project(
    usyseek
    VERSION 0.3.1
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
option(USYSEEK_USE_MIMALLOC "Replace default allocator with mimalloc" ON)
option(USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION "Enable FLTO/LTCG" ON)
option(USYSEEK_BUILD_MIGRATE "Build temporary Qdrant-to-Edge migration tool (usyseek-migrate.exe)" OFF)
cmake_dependent_option(USYSEEK_USE_STATIC_MSVCRT
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

set_tracked_cache_version(USYSEEK_USYLIBPP_VERSION "main" "usylibpp git tag/branch")
set_tracked_cache_version(USYSEEK_GLAZE_VERSION "v9.1.0" "glaze git tag")
set_tracked_cache_version(USYSEEK_PYBIND11_VERSION "v3.1.0" "pybind11 git tag")
set_tracked_cache_version(USYSEEK_MIMALLOC_VERSION "v3.5.3" "Version (git tag/branch) to use for mimalloc")
set_tracked_cache_version(USYSEEK_ORT_VERSION "1.31.1" "onnxruntime release")
set_tracked_cache_version(USYSEEK_EVERYTHING_VERSION "1.5.0.1424b" "Voidtools Everything release")
set_tracked_cache_version(USYSEEK_SQLITE_VERSION "3540100" "SQLite amalgamation release")

# Status Reports
if(WIN32)
    message(STATUS "[${PROJECT_NAME}] Using static MSVCRT: ${USYSEEK_USE_STATIC_MSVCRT} (USYSEEK_USE_STATIC_MSVCRT)")
endif()
message(STATUS "[${PROJECT_NAME}] Using interprocedural optimization: ${USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION} (USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION)")
message(STATUS "[${PROJECT_NAME}] Using mimalloc: ${USYSEEK_USE_MIMALLOC} (USYSEEK_USE_MIMALLOC)")
if(USYSEEK_USE_MIMALLOC)
    message(STATUS "[${PROJECT_NAME}] Using mimalloc version: ${USYSEEK_MIMALLOC_VERSION} (USYSEEK_MIMALLOC_VERSION)")
endif()

if(USYSEEK_USE_STATIC_MSVCRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

set(USYSEEK_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
set(USYSEEK_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${USYSEEK_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${USYSEEK_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${USYSEEK_BIN_DIR}")

# Downloads and extracts an archive once; ${OUT_VAR} receives the extraction directory.
function(usyseek_fetch_archive NAME URL OUT_VAR)
    set(archive "${USYSEEK_DEPS_DIR}/${NAME}.zip")
    set(dir "${USYSEEK_DEPS_DIR}/${NAME}")
    if(NOT EXISTS "${archive}")
        message(STATUS "[usyseek] Downloading ${URL}")
        file(DOWNLOAD "${URL}" "${archive}" SHOW_PROGRESS STATUS st)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${archive}")
            message(FATAL_ERROR "[usyseek] Failed to download ${URL}")
        endif()
    endif()
    if(NOT EXISTS "${dir}/.extracted")
        file(REMOVE_RECURSE "${dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
        file(TOUCH "${dir}/.extracted")
    endif()
    set(${OUT_VAR} "${dir}" PARENT_SCOPE)
endfunction()

##### Prebuilt binaries
# ONNX Runtime (CPU) - text tower in the search UI
usyseek_fetch_archive(onnxruntime-${USYSEEK_ORT_VERSION}
    "https://github.com/microsoft/onnxruntime/releases/download/v${USYSEEK_ORT_VERSION}/onnxruntime-win-x64-${USYSEEK_ORT_VERSION}.zip"
    ORT_EXTRACT_DIR)
set(ORT_ROOT "${ORT_EXTRACT_DIR}/onnxruntime-win-x64-${USYSEEK_ORT_VERSION}")

# Voidtools Everything - embedded into the indexer like dedupeify
usyseek_fetch_archive(everything-${USYSEEK_EVERYTHING_VERSION}
    "https://www.voidtools.com/Everything-${USYSEEK_EVERYTHING_VERSION}.x64.zip"
    EVERYTHING_DIR)
file(GLOB EVERYTHING_EXE "${EVERYTHING_DIR}/Everything*.exe")
list(GET EVERYTHING_EXE 0 EVERYTHING_EXE)

##### Rust staticlib (qdrant-edge + tokenizers + resvg)
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(RUST_DIR "${PROJECT_SOURCE_DIR}/rust")
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust_target")
set(RUST_STATIC_LIB "${RUST_TARGET_DIR}/release/usyseek_rust.lib")
set(RUST_OUTPUTS "${RUST_STATIC_LIB}")
set(RUST_DEPENDS "${RUST_DIR}/Cargo.toml" "${RUST_DIR}/Cargo.lock" "${RUST_DIR}/src/lib.rs" "${RUST_DIR}/src/edge_common.rs" "${RUST_DIR}/src/svg.rs")
set(RUST_CARGO_TARGETS --lib)
set(RUST_BUILD_COMMENT "Building usyseek_rust staticlib (qdrant-edge + tokenizers + resvg)...")
if(USYSEEK_BUILD_MIGRATE)
    set(RUST_MIGRATE_EXE "${RUST_TARGET_DIR}/release/usyseek-migrate.exe")
    list(APPEND RUST_OUTPUTS "${RUST_MIGRATE_EXE}")
    list(APPEND RUST_DEPENDS "${RUST_DIR}/src/bin/usyseek-migrate.rs")
    list(APPEND RUST_CARGO_TARGETS --bin usyseek-migrate)
    set(RUST_BUILD_COMMENT "Building usyseek_rust staticlib and usyseek-migrate (qdrant-edge + tokenizers + resvg)...")
endif()
add_custom_command(
    OUTPUT ${RUST_OUTPUTS}
    COMMAND "${CARGO_EXECUTABLE}" build --release ${RUST_CARGO_TARGETS} --manifest-path "${RUST_DIR}/Cargo.toml" --target-dir "${RUST_TARGET_DIR}"
    DEPENDS ${RUST_DEPENDS}
    WORKING_DIRECTORY "${RUST_DIR}"
    COMMENT "${RUST_BUILD_COMMENT}"
    VERBATIM
)
add_custom_target(usyseek_rust_build DEPENDS ${RUST_OUTPUTS})
add_library(usyseek_rust STATIC IMPORTED GLOBAL)
set_target_properties(usyseek_rust PROPERTIES IMPORTED_LOCATION "${RUST_STATIC_LIB}")
target_link_libraries(usyseek_rust INTERFACE ws2_32 userenv ntdll bcrypt advapi32 synchronization crypt32 secur32 ncrypt)
add_dependencies(usyseek_rust usyseek_rust_build)

##### FetchContent dependencies
set(USYLIBPP_ENABLE_WINDOWS ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINDOWS_UNICODE ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WIL ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINDOWS_IMAGING ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_TASK_DIALOG_DARK_MODE ON CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_WINTOAST OFF CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_VOIDTOOLS_EVERYTHING OFF CACHE BOOL "" FORCE)
set(USYLIBPP_ENABLE_VOIDTOOLS_EVERYTHING3 ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    usylibpp
    GIT_REPOSITORY https://github.com/usyless/usylibpp.git
    GIT_TAG "${USYSEEK_USYLIBPP_VERSION}"
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG "${USYSEEK_GLAZE_VERSION}"
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(
    sqlite3
    URL "https://sqlite.org/2026/sqlite-amalgamation-${USYSEEK_SQLITE_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(usylibpp glaze sqlite3)

add_library(usyseek_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
set_target_properties(usyseek_sqlite3 PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(usyseek_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(usyseek_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(usyseek_sqlite3 PRIVATE /W0)
endif()

##### Common compile settings
add_library(usyseek_common INTERFACE)
target_include_directories(usyseek_common INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/resources"
)
target_include_directories(usyseek_common SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/3rdparty")
target_link_libraries(usyseek_common INTERFACE usylibpp::usylibpp glaze::glaze version)
target_compile_definitions(usyseek_common INTERFACE
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
    target_compile_options(usyseek_common INTERFACE
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
    target_link_options(usyseek_common INTERFACE
        $<$<CONFIG:Debug>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
endif()

# mimalloc Configuration
if(USYSEEK_USE_MIMALLOC)
    target_compile_definitions(usyseek_common INTERFACE
        USYSEEK_USE_MIMALLOC
    )

    if(WIN32)
        if(NOT CMAKE_MSVC_RUNTIME_LIBRARY OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
            message(WARNING "[${PROJECT_NAME}] mimalloc static overloading on Windows requires the static CRT runtime (/MT). Currently CMAKE_MSVC_RUNTIME_LIBRARY is '${CMAKE_MSVC_RUNTIME_LIBRARY}' (defaults to /MD). Set CMAKE_MSVC_RUNTIME_LIBRARY before fetching ${PROJECT_NAME} to 'MultiThreaded' or 'MultiThreaded$<$<CONFIG:Debug>:Debug>' to ensure mimalloc takes precedence over the CRT.")
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
        GIT_TAG "${USYSEEK_MIMALLOC_VERSION}"
        EXCLUDE_FROM_ALL
        SYSTEM
    )
    FetchContent_MakeAvailable(mimalloc)

    target_link_libraries(usyseek_common INTERFACE
        mimalloc-obj
    )
endif()

##### usyseek.exe - launcher (tray, hotkey, process supervision)
add_executable(usyseek WIN32
    src/launcher.cpp
    resources/app.rc
)
set_target_properties(usyseek PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(usyseek PRIVATE usyseek_common shell32 user32 ole32)

##### usyseek-ui.exe - search pill + settings (Direct2D, ONNX Runtime text tower)
add_executable(usyseek-ui WIN32
    src/ui.cpp
    resources/app.rc
)
set_target_properties(usyseek-ui PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(usyseek-ui SYSTEM PRIVATE "${ORT_ROOT}/include")
target_link_libraries(usyseek-ui PRIVATE
    usyseek_common
    usyseek_rust
    usyseek_sqlite3
    "${ORT_ROOT}/lib/onnxruntime.lib"
    d2d1 dwrite d3d11 d3dcompiler dxgi dxguid dcomp dwmapi windowscodecs mfplat mfuuid
    shell32 shlwapi shcore ole32 oleaut32 user32 gdi32 imm32 uxtheme propsys winhttp version
)
add_custom_command(TARGET usyseek-ui POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:usyseek-ui>/onnxruntime.dll"
)

##### usyseek-indexer.exe - scanning + embedding (Everything, WIC, Media Foundation, shared-memory Python worker)
set(USYSEEK_RES_DIR "${PROJECT_SOURCE_DIR}/resources")
configure_file("${PROJECT_SOURCE_DIR}/resources/indexer.rc.in" "${CMAKE_BINARY_DIR}/indexer.rc" @ONLY)
add_executable(usyseek-indexer
    src/indexer.cpp
    "${CMAKE_BINARY_DIR}/indexer.rc"
)
set_target_properties(usyseek-indexer PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(usyseek-indexer PRIVATE
    usyseek_common
    usyseek_rust
    usyseek_sqlite3
    windowscodecs mfplat mfreadwrite mfuuid propsys
    shell32 shlwapi ole32 oleaut32 user32
)

##### Runtime layout next to the executables
set(USYSEEK_MIGRATE_COPY_CMD "")
if(USYSEEK_BUILD_MIGRATE)
    set(USYSEEK_MIGRATE_COPY_CMD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RUST_MIGRATE_EXE}" "${USYSEEK_BIN_DIR}/usyseek-migrate.exe")
endif()
add_custom_target(usyseek_runtime ALL
    DEPENDS usyseek_rust_build
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USYSEEK_BIN_DIR}/python"
    ${USYSEEK_MIGRATE_COPY_CMD}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/python/usyseek_vision.py" "${USYSEEK_BIN_DIR}/python/usyseek_vision.py"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/python/requirements.txt" "${USYSEEK_BIN_DIR}/python/requirements.txt"
    VERBATIM
)
