cmake_minimum_required(VERSION 3.27)
project(uv-co)

include(CTest)
include(CMakePackageConfigHelpers)

find_package(PkgConfig)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

# Allow using a custom libuv installation.
link_directories(BEFORE /usr/local/lib64)

find_package(Boost 1.80 COMPONENTS log log_setup program_options REQUIRED)
find_package(CURL)
find_package(GTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 23)
add_compile_options(-Wall -pipe)

find_program(MOLD mold)
if(MOLD)
    add_link_options(-fuse-ld=mold)
endif()

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

# Unit tests

if(GTest_FOUND)
    enable_testing()

    add_library(test-util STATIC test/test_util.cc)
    target_include_directories(test-util PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-util PRIVATE uv-co-lib)

    foreach(TESTCASE IN ITEMS async_work channel fs generator loop name_resolution promise select stream timer udp uds tcp misc)
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

else()
    message(WARNING "GTest not found, skipping tests")
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
