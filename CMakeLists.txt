cmake_minimum_required(VERSION 3.27)
project(uv-co)

include(CTest)
find_package(GTest)

find_package(PkgConfig)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
add_compile_options(-std=c++2b -Wall -fno-inline -pipe)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

add_library(event-loop-lib STATIC
    src/close.cc
    src/internal_utils.cc
    src/name_resolution.cc
    src/promise.cc
    src/stream.cc
    src/tcp.cc
    src/timer.cc
    src/udp.cc

    src/event_loop.cc)
target_link_libraries(event-loop-lib PRIVATE ${UV_LIBRARIES} ${FMT_LIBRARIES})
target_include_directories(event-loop-lib PRIVATE ${UV_INCLUDEDIR} ${FMT_INCLUDEDIR})

if(ENABLE_COVERAGE)
    target_compile_options(event-loop-lib PRIVATE "--coverage")
    target_link_libraries(event-loop-lib PRIVATE gcov)
endif()

add_executable(test-event-loop src/test_event_loop.cc)
target_link_libraries(test-event-loop event-loop-lib)

# Unit tests

add_library(test-util STATIC test/test_util.cc)
target_include_directories(test-util PRIVATE src/)

add_executable(timer-test test/timer_test.cc)
target_include_directories(timer-test PRIVATE src/)
target_link_libraries(timer-test PRIVATE event-loop-lib test-util GTest::gtest_main)
add_test(NAME timer-test COMMAND timer-test)

add_executable(udp-test test/udp_test.cc)
target_include_directories(udp-test PRIVATE src/)
target_link_libraries(udp-test PRIVATE event-loop-lib test-util GTest::gtest_main)
add_test(NAME udp-test COMMAND udp-test)

add_executable(tcp-test test/tcp_test.cc)
target_include_directories(tcp-test PRIVATE src/)
target_link_libraries(tcp-test PRIVATE event-loop-lib test-util GTest::gtest_main)
add_test(NAME tcp-test COMMAND tcp-test)

# Coverage
add_custom_target(coverage
    COMMAND lcov --directory . --capture --include */uv-co/* --output-file app.info
    COMMAND genhtml -o coverage app.info
    COMMAND lcov --directory . --zerocounters
    )
