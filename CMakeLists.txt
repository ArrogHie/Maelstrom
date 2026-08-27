cmake_minimum_required(VERSION 3.20)

project(Maelstrom
    VERSION 0.1.0
    LANGUAGES CXX
)

# --- C++ 标准（项目级；目标级 target_compile_features 更优，W2 再切）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # 用 -std=c++17，不是 gnu++17

# --- 导出 compile_commands.json 给 clang-tidy / 编辑器
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 选项
option(MAELSTROM_BUILD_TESTS       "Build tests"    ON)
option(MAELSTROM_ENABLE_CLANG_TIDY "Run clang-tidy" OFF)

# --- clang-tidy 集成（CMake 内建变量，必须在 add_* 之前设置）
if(MAELSTROM_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-p;${CMAKE_BINARY_DIR}")
    else()
        message(WARNING "clang-tidy not found")
    endif()
endif()

# --- 统一的告警开关（函数复用，目标化）
function(maelstrom_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
    endif()
endfunction()

# --- 主库
add_library(maelstrom STATIC src/order_book.cpp
    include/Maelstrom/order_book.h)
add_library(maelstrom::maelstrom ALIAS maelstrom)

target_include_directories(maelstrom PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
maelstrom_target_warnings(maelstrom)

# --- 测试
if(MAELSTROM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()