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

add_library(uv-co-internal STATIC
    src/internal/internal_utils.cc
)
target_include_directories(uv-co-internal PRIVATE ${FMT_INCLUDEDIR})

add_library(uv-co-promise STATIC
    src/promise/multipromise.cc
    src/promise/promise.cc
    src/promise/promise_core.cc
)
target_link_libraries(uv-co-promise PRIVATE uv-co-internal)
target_include_directories(uv-co-promise PRIVATE src/)

add_library(uv-co-lib STATIC
    src/channel.cc
    src/close.cc
    src/name_resolution.cc
    src/pipe.cc
    src/scheduler.cc
    src/stream.cc
    src/tcp.cc
    src/timer.cc
    src/udp.cc
)
target_link_libraries(uv-co-lib PRIVATE uv-co-internal ${UV_LIBRARIES} ${FMT_LIBRARIES})
target_link_libraries(uv-co-lib PUBLIC uv-co-promise)
target_include_directories(uv-co-lib PRIVATE ${UV_INCLUDEDIR} ${FMT_INCLUDEDIR})
target_include_directories(uv-co-lib PUBLIC src/)

if(ENABLE_COVERAGE)
    target_compile_options(uv-co-internal PRIVATE "--coverage")
    target_link_libraries(uv-co-internal PRIVATE gcov)
    target_compile_options(uv-co-promise PRIVATE "--coverage")
    target_link_libraries(uv-co-promise PRIVATE gcov)
    target_compile_options(uv-co-lib PRIVATE "--coverage")
    target_link_libraries(uv-co-lib PRIVATE gcov)
endif()

# Unit tests

add_library(test-util STATIC test/test_util.cc)
target_include_directories(test-util PRIVATE src/)
target_link_libraries(test-util PRIVATE uv-co-lib)

foreach(TESTCASE IN ITEMS channel timer udp tcp misc)
    add_executable(${TESTCASE}-test test/${TESTCASE}_test.cc)
    target_link_libraries(${TESTCASE}-test PRIVATE uv-co-lib test-util GTest::gtest_main)
    add_test(NAME ${TESTCASE}-test COMMAND ${TESTCASE}-test)

    if(ENABLE_COVERAGE)
        target_compile_options(${TESTCASE}-test PRIVATE "--coverage")
        target_link_libraries(${TESTCASE}-test PRIVATE gcov)
    endif()

endforeach()

foreach(TESTBINARY IN ITEMS tcp-broadcaster multicast)
    add_executable(test-${TESTBINARY} test/${TESTBINARY}.exe.cc)
    target_link_libraries(test-${TESTBINARY} PRIVATE uv-co-lib boost_program_options)
endforeach()

# Coverage
add_custom_target(coverage
    COMMAND lcov --directory . --capture --include */uv-co/* --output-file app.info --ignore-errors mismatch
    COMMAND genhtml -o coverage app.info
    COMMAND lcov --directory . --zerocounters
    )
