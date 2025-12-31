cmake_minimum_required(VERSION 3.20)
project(main VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE sources *.cpp)

add_executable(${PROJECT_NAME} main.cpp)

find_package(Vulkan REQUIRED)

# target link libraries
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/includes)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Vulkan::Vulkan
)