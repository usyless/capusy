cmake_minimum_required(VERSION 3.28)
project(capusy_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

set(CAPUSY_TEST_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/sqlite3-src")
set(CAPUSY_TEST_USYLIBPP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/usylibpp-src/include")
set(CAPUSY_TEST_GLAZE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/glaze-src/include")

add_library(capusy_tests_sqlite3 STATIC "${CAPUSY_TEST_SQLITE_DIR}/sqlite3.c")
set_target_properties(capusy_tests_sqlite3 PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(capusy_tests_sqlite3 SYSTEM PUBLIC "${CAPUSY_TEST_SQLITE_DIR}")
target_compile_definitions(capusy_tests_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(capusy_tests_sqlite3 PRIVATE /W0)
endif()

add_executable(core_test core_test.cpp)
target_include_directories(core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(core_test SYSTEM PRIVATE "${CAPUSY_TEST_USYLIBPP_INCLUDE_DIR}" "${CAPUSY_TEST_GLAZE_INCLUDE_DIR}")
target_compile_definitions(core_test PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(core_test PRIVATE capusy_tests_sqlite3)
if(MSVC)
    target_compile_options(core_test PRIVATE /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
endif()
add_test(NAME core_test COMMAND core_test)
