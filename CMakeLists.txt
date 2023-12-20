cmake_minimum_required(VERSION 3.27)
project(uv-co)

include(CTest)
find_package(GTest)

find_package(PkgConfig)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
add_compile_options(-std=c++2b -Wall -fno-inline -pipe -g)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

add_library(event-loop-lib STATIC
    src/channel.cc
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
target_include_directories(event-loop-lib PUBLIC src/)

if(ENABLE_COVERAGE)
    target_compile_options(event-loop-lib PRIVATE "--coverage")
    target_link_libraries(event-loop-lib PRIVATE gcov)
endif()

add_executable(test-event-loop src/test_event_loop.cc)
target_link_libraries(test-event-loop event-loop-lib)

# Unit tests

add_library(test-util STATIC test/test_util.cc)
target_include_directories(test-util PRIVATE src/)
target_link_libraries(test-util PRIVATE event-loop-lib)

foreach(TESTCASE IN ITEMS channel timer udp tcp misc)
    add_executable(${TESTCASE}-test test/${TESTCASE}_test.cc)
    target_link_libraries(${TESTCASE}-test PRIVATE event-loop-lib test-util GTest::gtest_main)
    add_test(NAME ${TESTCASE}-test COMMAND ${TESTCASE}-test)

    if(ENABLE_COVERAGE)
        target_compile_options(${TESTCASE}-test PRIVATE "--coverage")
        target_link_libraries(${TESTCASE}-test PRIVATE gcov)
    endif()

endforeach()

foreach(TESTBINARY IN ITEMS tcp-broadcaster multicast)
    add_executable(test-${TESTBINARY} test/${TESTBINARY}.exe.cc)
    target_link_libraries(test-${TESTBINARY} PRIVATE event-loop-lib boost_program_options)
endforeach()

# Coverage
add_custom_target(coverage
    COMMAND lcov --directory . --capture --include */uv-co/* --output-file app.info
    COMMAND genhtml -o coverage app.info
    COMMAND lcov --directory . --zerocounters
    )
