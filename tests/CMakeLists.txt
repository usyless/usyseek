cmake_minimum_required(VERSION 3.28)
project(usyseek_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

set(USYSEEK_TEST_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/sqlite3-src")
if(NOT EXISTS "${USYSEEK_TEST_SQLITE_DIR}/sqlite3.c")
    include(FetchContent)
    FetchContent_Declare(
        sqlite3
        URL "https://sqlite.org/2026/sqlite-amalgamation-3540100.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite3)
    set(USYSEEK_TEST_SQLITE_DIR "${sqlite3_SOURCE_DIR}")
endif()

add_library(usyseek_tests_sqlite3 STATIC "${USYSEEK_TEST_SQLITE_DIR}/sqlite3.c")
set_target_properties(usyseek_tests_sqlite3 PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(usyseek_tests_sqlite3 SYSTEM PUBLIC "${USYSEEK_TEST_SQLITE_DIR}")
target_compile_definitions(usyseek_tests_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(usyseek_tests_sqlite3 PRIVATE /W0)
endif()

file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
foreach(src IN LISTS test_sources)
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    if(name STREQUAL "tag_store_test")
        target_link_libraries(${name} PRIVATE usyseek_tests_sqlite3)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()
