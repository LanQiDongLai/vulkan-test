cmake_minimum_required(VERSION 3.20)
project(main VERSION 0.0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(${PROJECT_NAME} main.cpp)

find_package(Vulkan REQUIRED)

# target link libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Vulkan::Vulkan
)