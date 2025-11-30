cmake_minimum_required(VERSION 3.27)
project(uv-co)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # This can cause trouble if compiling libraries for system-wide use.
    # Generally you must use the same compiler for application code as was used
    # to build the uvco code.
    if(NOT DEFINED ENABLE_LTO)
        set(ENABLE_LTO 1)
    endif()
    if(NOT DEFINED ENABLE_ASAN)
        set(ENABLE_ASAN 0)
    endif()
    if(NOT DEFINED ENABLE_COVERAGE)
        set(ENABLE_COVERAGE 0)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED ENABLE_LTO)
        set(ENABLE_LTO 0)
    endif()
    if(NOT DEFINED ENABLE_ASAN)
        set(ENABLE_ASAN 1)
    endif()
    if(NOT DEFINED ENABLE_COVERAGE)
        set(ENABLE_COVERAGE 1)
    endif()
else()
    if(NOT DEFINED ENABLE_LTO)
        set(ENABLE_LTO 0)
    endif()
    if(NOT DEFINED ENABLE_ASAN)
        set(ENABLE_ASAN 0)
    endif()
    if(NOT DEFINED ENABLE_COVERAGE)
        set(ENABLE_COVERAGE 0)
    endif()
endif()

MESSAGE(STATUS "LTO ${ENABLE_LTO} ASAN ${ENABLE_ASAN} COVERAGE ${ENABLE_COVERAGE}")

include(CTest)
include(CMakePackageConfigHelpers)

find_package(PkgConfig)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

# Allow using a custom libuv installation.
link_directories(BEFORE /usr/local/lib64 /usr/local/lib)

find_package(Boost 1.80
  CONFIG
  COMPONENTS
    log
    log_setup
    program_options
  REQUIRED)
find_package(benchmark)
find_package(CURL)
find_package(libpqxx)
find_package(GTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 23)
add_compile_options(
  -pipe
  -fno-omit-frame-pointer
  -Wall
  -Wextra
  -Wpedantic
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fcolor-diagnostics)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-fdiagnostics-color=always)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENABLE_COVERAGE)
    add_compile_options(--coverage)
    add_link_options(--coverage -lgcov)
endif()

if (ENABLE_LTO)
    add_compile_options(-flto)
    add_link_options(-flto)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_link_options(-fuse-ld=lld)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # Works by default, usually.
    endif()
endif()

# Core utilities not relying on uv-co-promise or uv-co-lib.
add_library(uv-co-base STATIC
    uvco/exception.cc
    uvco/internal/internal_utils.cc
    uvco/loop/loop.cc
    uvco/loop/scheduler.cc
)
target_sources(uv-co-base INTERFACE
    FILE_SET HEADERS
    TYPE HEADERS
    FILES
    uvco/exception.h
    uvco/internal/internal_utils.h
    uvco/loop/loop.h
    uvco/loop/scheduler.h
)
target_include_directories(uv-co-base PRIVATE ${FMT_INCLUDEDIR} .)
install(TARGETS uv-co-base EXPORT uvco FILE_SET HEADERS DESTINATION include)

add_library(uv-co-promise STATIC
    uvco/promise/multipromise.cc
    uvco/promise/promise.cc
    uvco/promise/promise_core.cc
)
target_sources(uv-co-promise INTERFACE
    FILE_SET HEADERS
    TYPE HEADERS
    FILES
    uvco/promise/multipromise.h
    uvco/promise/promise.h
    uvco/promise/promise_core.h
)
target_link_libraries(uv-co-promise PRIVATE uv-co-base)
target_include_directories(uv-co-promise PRIVATE .)
install(TARGETS uv-co-promise EXPORT uvco FILE_SET HEADERS DESTINATION include)

add_library(uv-co-lib STATIC
    uvco/async_work.cc
    uvco/channel.cc
    uvco/close.cc
    uvco/combinators.cc
    uvco/fs.cc
    uvco/name_resolution.cc
    uvco/pipe.cc
    uvco/run.cc
    uvco/stream.cc
    uvco/stream_server_base_impl.cc
    uvco/tcp.cc
    uvco/tcp_stream.cc
    uvco/timer.cc
    uvco/udp.cc
    uvco/uds.cc
    uvco/uds_stream.cc
)

target_sources(uv-co-lib INTERFACE
    FILE_SET HEADERS
    TYPE HEADERS
    FILES
    uvco/async_work.h
    uvco/channel.h
    uvco/close.h
    uvco/combinators.h
    uvco/fs.h
    uvco/name_resolution.h
    uvco/pipe.h
    uvco/run.h
    uvco/stream.h
    uvco/stream_server_base.h
    uvco/stream_server_base_impl.h
    uvco/tcp.h
    uvco/tcp_stream.h
    uvco/timer.h
    uvco/udp.h
    uvco/uds.h
    uvco/uds_stream.h
)

target_link_libraries(uv-co-lib
    PUBLIC
    uv-co-base
    uv-co-promise
    PRIVATE
    ${UV_LIBRARIES}
    ${FMT_LIBRARIES}
)

target_include_directories(uv-co-lib PRIVATE ${UV_INCLUDEDIR} ${FMT_INCLUDEDIR} .)
install(TARGETS uv-co-lib EXPORT uvco FILE_SET HEADERS DESTINATION include)

# Package configuration

install(EXPORT uvco FILE UvcoTargets.cmake DESTINATION lib/cmake/uvco NAMESPACE Uvco:: )
export(EXPORT uvco FILE UvcoTargets.cmake NAMESPACE Uvco::)
configure_package_config_file(Config.cmake.in UvcoConfig.cmake INSTALL_DESTINATION lib/cmake/uvco)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/UvcoConfig.cmake"
    DESTINATION lib/cmake/uvco
)

# Optional integrations

if(CURL_FOUND)
    add_library(uv-co-curl STATIC
        uvco/integrations/curl/curl.cc
    )
    target_sources(uv-co-curl INTERFACE
        FILE_SET HEADERS
        TYPE HEADERS
        BASE_DIRS uvco/
        FILES
        uvco/integrations/curl/curl.h
    )
    target_link_libraries(uv-co-curl PUBLIC uv-co-lib CURL::libcurl)
    target_include_directories(uv-co-curl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    install(TARGETS uv-co-curl)
    install(TARGETS uv-co-curl FILE_SET HEADERS DESTINATION include/uvco)
endif()

if(libpqxx_FOUND)
    add_library(uv-co-pqxx STATIC
        uvco/integrations/pqxx/pqxx.cc
    )
    target_sources(uv-co-pqxx INTERFACE
        FILE_SET HEADERS
        TYPE HEADERS
        BASE_DIRS uvco/
        FILES
        uvco/integrations/pqxx/pqxx.h
    )
    target_link_libraries(uv-co-pqxx PUBLIC uv-co-lib libpqxx::pqxx)
    target_include_directories(uv-co-pqxx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    install(TARGETS uv-co-pqxx)
    install(TARGETS uv-co-pqxx FILE_SET HEADERS DESTINATION include/uvco)
endif()

# Unit tests

if(GTest_FOUND)
    enable_testing()

    add_library(test-util STATIC test/test_util.cc)
    target_include_directories(test-util PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-util PRIVATE uv-co-lib)

    add_library(uv-co-example-http STATIC
      uvco/examples/http_server.cc
  )

    target_sources(uv-co-example-http INTERFACE
      FILE_SET HEADERS
      TYPE HEADERS
      FILES
      uvco/examples/http_server.h
  )

    target_link_libraries(uv-co-example-http
      PUBLIC
      uv-co-lib
  )

    target_include_directories(uv-co-example-http PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    install(TARGETS uv-co-example-http EXPORT uvco FILE_SET HEADERS DESTINATION include)

    add_executable(http-server benchmark/http_server.cc)
    target_link_libraries(http-server
    PRIVATE
    uv-co-example-http
    Boost::program_options
  )

    foreach(TESTCASE IN ITEMS async_work channel combinators fs generator loop name_resolution promise select stream timer udp uds tcp misc)
        add_executable(${TESTCASE}-test test/${TESTCASE}_test.cc)
        target_link_libraries(${TESTCASE}-test PRIVATE uv-co-lib test-util GTest::gtest_main)
        add_test(NAME ${TESTCASE}-test COMMAND ${TESTCASE}-test)

        if(ENABLE_COVERAGE)
            target_compile_options(${TESTCASE}-test PRIVATE "--coverage")
            target_link_libraries(${TESTCASE}-test PRIVATE gcov)
        endif()
    endforeach()

    add_executable(http_server-test test/http_server_test.cc)
    target_link_libraries(http_server-test
    PRIVATE
    uv-co-example-http
    test-util
    GTest::gtest_main
  )
    add_test(NAME http_server-test COMMAND http_server-test)

    if(ENABLE_COVERAGE)
        target_compile_options(http_server-test PRIVATE "--coverage")
        target_link_libraries(http_server-test PRIVATE gcov)
    endif()

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
    if(libpqxx_FOUND)
        add_executable(pqxx-test test/pqxx_test.cc)
        target_link_libraries(pqxx-test PRIVATE uv-co-pqxx test-util GTest::gtest_main)
        add_test(NAME pqxx-test COMMAND pqxx-test)
    endif()

else()
    message(WARNING "GTest not found, skipping tests")
endif()

# Benchmarks

if (benchmark_FOUND)
    foreach(BENCHMARKCASE IN ITEMS stream promise)
        add_executable(${BENCHMARKCASE}-benchmark benchmark/${BENCHMARKCASE}_bench.cc)
        target_link_libraries(${BENCHMARKCASE}-benchmark
      PRIVATE
        uv-co-lib
        benchmark::benchmark
        benchmark::benchmark_main
        GTest::gtest
    )
    endforeach()
else()
    message(WARNING "Google Benchmark not found, skipping benchmarks")
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
    COMMAND gcovr --html-nested -o coverage/uvco.html --gcov-executable ${GCOV_BINARY} -r .. --merge-mode-functions=separate
    COMMAND find . -name *.gcda -delete
)
add_custom_target(grcov
    COMMAND mkdir -p grcov
    COMMAND grcov . -s .. --binary-path . -t html --branch --ignore-not-existing -o grcov --llvm
    COMMAND find . -name *.gcda -delete
)
