cmake_minimum_required(VERSION 2.8)
project(flash_tool C)

# set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "-std=gnu99")

include_directories(
    ./src/
)

add_executable(flash_tool
    src/main.c
)
target_link_libraries( flash_tool )
install(TARGETS flash_tool RUNTIME DESTINATION /usr/bin/)
