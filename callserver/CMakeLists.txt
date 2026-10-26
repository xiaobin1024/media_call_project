cmake_minimum_required(VERSION 3.12)
project(call_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AddressSanitizer for debugging crashes on Linux
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer -g")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
endif()

find_package(Threads REQUIRED)

add_executable(call_server
    src/main.cpp
    src/signaling_server.cpp
    src/session.cpp
)

target_include_directories(call_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(call_server PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(call_server PRIVATE ws2_32)
endif()
