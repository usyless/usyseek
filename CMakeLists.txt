cmake_minimum_required(VERSION 3.31...4.4.2)

set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "" FORCE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)

project(
    usyseek
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
option(USYSEEK_USE_INTERPROCEDURAL_OPTIMIZATION "Enable LTCG" ON)
cmake_dependent_option(USYSEEK_USE_STATIC_MSVCRT "Use the static MSVCRT" ON "MSVC" OFF)

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
set_tracked_cache_version(USYSEEK_ORT_VERSION "1.31.1" "onnxruntime release")
set_tracked_cache_version(USYSEEK_QDRANT_VERSION "v1.20.0" "qdrant release")
set_tracked_cache_version(USYSEEK_EVERYTHING_VERSION "1.5.0.1424b" "Voidtools Everything release")

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

# Qdrant server - reuse a local copy (USYSEEK_QDRANT_EXE) or download the release
set(USYSEEK_QDRANT_EXE "${PROJECT_SOURCE_DIR}/../usybooru/qdrant/qdrant.exe" CACHE FILEPATH "Existing qdrant.exe to bundle")
if(NOT EXISTS "${USYSEEK_QDRANT_EXE}")
    usyseek_fetch_archive(qdrant-${USYSEEK_QDRANT_VERSION}
        "https://github.com/qdrant/qdrant/releases/download/${USYSEEK_QDRANT_VERSION}/qdrant-x86_64-pc-windows-msvc.zip"
        QDRANT_DIR)
    file(GLOB_RECURSE USYSEEK_QDRANT_EXE "${QDRANT_DIR}/qdrant.exe")
endif()

##### Rust staticlib (qdrant gRPC client + tokenizers)
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(RUST_DIR "${PROJECT_SOURCE_DIR}/rust")
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust_target")
set(RUST_STATIC_LIB "${RUST_TARGET_DIR}/release/usyseek_rust.lib")
add_custom_command(
    OUTPUT "${RUST_STATIC_LIB}"
    COMMAND "${CARGO_EXECUTABLE}" build --release --manifest-path "${RUST_DIR}/Cargo.toml" --target-dir "${RUST_TARGET_DIR}"
    DEPENDS "${RUST_DIR}/Cargo.toml" "${RUST_DIR}/src/lib.rs"
    WORKING_DIRECTORY "${RUST_DIR}"
    COMMENT "Building usyseek_rust staticlib (qdrant-client + tokenizers)..."
    VERBATIM
)
add_custom_target(usyseek_rust_build DEPENDS "${RUST_STATIC_LIB}")
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
set(USYLIBPP_ENABLE_VOIDTOOLS_EVERYTHING ON CACHE BOOL "" FORCE)

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
FetchContent_MakeAvailable(usylibpp glaze)

# Embedded CPython for the vision tower (torch). Must match the venv's major.minor version.
set(Python_FIND_REGISTRY NEVER)
set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Embed)
message(STATUS "[usyseek] Embedding Python ${Python_VERSION_MAJOR}.${Python_VERSION_MINOR} from ${Python_EXECUTABLE}")

FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG "${USYSEEK_PYBIND11_VERSION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(pybind11)

##### Common compile settings
add_library(usyseek_common INTERFACE)
target_include_directories(usyseek_common INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/resources"
)
target_include_directories(usyseek_common SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/3rdparty")
target_link_libraries(usyseek_common INTERFACE usylibpp::usylibpp glaze::glaze)
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
    "${ORT_ROOT}/lib/onnxruntime.lib"
    d2d1 dwrite d3d11 dxgi dxguid dcomp dwmapi windowscodecs
    shell32 shlwapi shcore ole32 oleaut32 user32 gdi32 imm32 uxtheme propsys
)
add_custom_command(TARGET usyseek-ui POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:usyseek-ui>/onnxruntime.dll"
)

##### usyseek-indexer.exe - scanning + embedding (Everything, WIC, Media Foundation, embedded torch)
set(USYSEEK_RES_DIR "${PROJECT_SOURCE_DIR}/resources")
configure_file("${PROJECT_SOURCE_DIR}/resources/indexer.rc.in" "${CMAKE_BINARY_DIR}/indexer.rc" @ONLY)
add_executable(usyseek-indexer
    src/indexer.cpp
    "${CMAKE_BINARY_DIR}/indexer.rc"
)
set_target_properties(usyseek-indexer PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(usyseek-indexer PRIVATE "${pybind11_SOURCE_DIR}/include")
target_link_libraries(usyseek-indexer PRIVATE
    usyseek_common
    usyseek_rust
    pybind11::embed
    windowscodecs mfplat mfreadwrite mfuuid propsys
    shell32 shlwapi ole32 oleaut32 user32
)
target_compile_definitions(usyseek-indexer PRIVATE
    USYSEEK_PYTHON_VERSION=L"${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}"
)
target_compile_definitions(usyseek-ui PRIVATE
    USYSEEK_PYTHON_VERSION=L"${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}"
)

##### Runtime layout next to the executables
add_custom_target(usyseek_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USYSEEK_BIN_DIR}/qdrant" "${USYSEEK_BIN_DIR}/config" "${USYSEEK_BIN_DIR}/python"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${USYSEEK_QDRANT_EXE}" "${USYSEEK_BIN_DIR}/qdrant/qdrant.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/config/qdrant.yaml" "${USYSEEK_BIN_DIR}/config/qdrant.yaml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/python/usyseek_vision.py" "${USYSEEK_BIN_DIR}/python/usyseek_vision.py"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/python/requirements.txt" "${USYSEEK_BIN_DIR}/python/requirements.txt"
    VERBATIM
)
