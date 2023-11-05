cmake_minimum_required(VERSION 3.27)
project(uv-co)

find_package(PkgConfig)

pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(UV REQUIRED libuv)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "-std=c++2b -Wall")

add_library(event-loop STATIC src/event_loop.cc)
target_link_libraries(event-loop PRIVATE ${UV_LIBRARIES} ${FMT_LIBRARIES})
target_include_directories(event-loop PRIVATE ${UV_INCLUDEDIR} ${FMT_INCLUDEDIR})
target_compile_options(event-loop PRIVATE)


add_executable(test-event-loop src/test_event_loop.cc)
target_link_libraries(test-event-loop event-loop)

