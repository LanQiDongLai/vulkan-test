cmake_minimum_required(VERSION 3.20)
project(main VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(${PROJECT_NAME} main.cpp)

find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)

# 用 glslc 把 GLSL 编译成 SPIR-V，输出到 build/shaders
find_program(GLSLC_EXECUTABLE NAMES glslc)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "找不到 glslc，请安装 shaderc (Arch: pacman -S shaderc)")
endif()

set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_DIR})

set(SHADER_SOURCES triangle.vert triangle.frag)
set(SHADER_SPV)
foreach(shader IN LISTS SHADER_SOURCES)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
    set(output ${SHADER_DIR}/${shader}.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${GLSLC_EXECUTABLE} ${source} -o ${output}
        DEPENDS ${source}
        COMMENT "编译着色器 ${shader}"
        VERBATIM)
    list(APPEND SHADER_SPV ${output})
endforeach()

add_custom_target(shaders ALL DEPENDS ${SHADER_SPV})
add_dependencies(${PROJECT_NAME} shaders)

# target compile definition: 运行时从哪里加载 .spv
target_compile_definitions(${PROJECT_NAME} PRIVATE SHADER_DIR="${SHADER_DIR}")

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Vulkan::Vulkan
        SDL2::SDL2
)