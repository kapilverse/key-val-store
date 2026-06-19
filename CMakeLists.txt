cmake_minimum_required(VERSION 3.15)
project(kvstore VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /WX)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror -O2)
endif()

add_library(kvlib STATIC
    src/wal.cpp
    src/memtable.cpp
    src/sstable.cpp
    src/db.cpp
)
target_include_directories(kvlib PUBLIC include)

add_executable(kvstore src/main.cpp)
target_link_libraries(kvstore PRIVATE kvlib)
