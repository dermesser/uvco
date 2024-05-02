cmake_minimum_required(VERSION 3.27)
project(uv-co)

include(CTest)

find_package(PkgConfig)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

link_directories(BEFORE /usr/local/lib64)

find_package(Boost 1.80 COMPONENTS log log_setup program_options REQUIRED)
find_package(CURL)
find_package(GTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
add_compile_options(-std=c++2b -Wall -fno-inline -pipe)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fcolor-diagnostics)
else()
    add_compile_options(-fdiagnostics-color=always)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_COVERAGE)
    add_compile_options(--coverage)
    add_link_options(--coverage -lgcov)
endif()

# Core utilities not relying on uv-co-promise or uv-co-lib.
add_library(uv-co-internal STATIC
    src/internal/internal_utils.cc
    src/loop/loop.cc
    src/loop/scheduler.cc
)
target_include_directories(uv-co-internal PRIVATE ${FMT_INCLUDEDIR} src/)

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
    src/exception.cc
    src/name_resolution.cc
    src/pipe.cc
    src/run.cc
    src/stream.cc
    src/stream_server_base_impl.cc
    src/tcp.cc
    src/tcp_stream.cc
    src/timer.cc
    src/udp.cc
    src/uds.cc
    src/uds_stream.cc
)
target_link_libraries(uv-co-lib PRIVATE uv-co-internal ${UV_LIBRARIES} ${FMT_LIBRARIES})
target_link_libraries(uv-co-lib PUBLIC uv-co-promise)
target_include_directories(uv-co-lib PRIVATE ${UV_INCLUDEDIR} ${FMT_INCLUDEDIR})
target_include_directories(uv-co-lib PUBLIC src/)

# Optional integrations

if(CURL_FOUND)
    add_library(uv-co-curl STATIC
        src/integrations/curl/curl.cc
    )
    target_link_libraries(uv-co-curl PUBLIC uv-co-lib CURL::libcurl)
endif()

# Unit tests

add_library(test-util STATIC test/test_util.cc)
target_include_directories(test-util PRIVATE src/)
target_link_libraries(test-util PRIVATE uv-co-lib)

foreach(TESTCASE IN ITEMS channel generator loop name_resolution promise stream timer udp uds tcp misc)
    add_executable(${TESTCASE}-test test/${TESTCASE}_test.cc)
    target_link_libraries(${TESTCASE}-test PRIVATE uv-co-lib test-util GTest::gtest_main)
    add_test(NAME ${TESTCASE}-test COMMAND ${TESTCASE}-test)

    if(ENABLE_COVERAGE)
        target_compile_options(${TESTCASE}-test PRIVATE "--coverage")
        target_link_libraries(${TESTCASE}-test PRIVATE gcov)
    endif()
endforeach()

foreach(TESTBINARY IN ITEMS tcp-broadcaster multicast http10 memcached-impl)
    add_executable(test-${TESTBINARY} test/${TESTBINARY}.exe.cc)
    target_link_libraries(test-${TESTBINARY} PRIVATE uv-co-lib Boost::program_options)
endforeach()

target_link_libraries(test-memcached-impl PRIVATE Boost::log Boost::log_setup)

if(CURL_FOUND)
    add_executable(curl-test test/curl_test.cc)
    target_link_libraries(curl-test PRIVATE uv-co-curl test-util GTest::gtest_main)
    add_test(NAME curl-test COMMAND curl-test)
endif()

# Coverage

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(LLVM_COV_BINARY NAMES llvm-cov llvm-cov-18)
    set(GCOV_BINARY "${LLVM_COV_BINARY} gcov")
else()
    set(GCOV_BINARY "gcov")
endif()

add_custom_target(coverage
    COMMAND mkdir -p coverage
    COMMAND gcovr --html-nested -o coverage/uvco.html --gcov-executable ${GCOV_BINARY} -r ..
)
add_custom_target(grcov
    COMMAND mkdir -p grcov
    COMMAND grcov . -s .. --binary-path . -t html --branch --ignore-not-existing -o grcov
)
