cmake_minimum_required(VERSION 3.27)
project(uv-co)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

add_library(event-loop STATIC src/event_loop.cc)
target_link_libraries(event-loop PRIVATE uv)

add_executable(test-event-loop src/test_event_loop.cc)
target_link_libraries(test-event-loop event-loop)

