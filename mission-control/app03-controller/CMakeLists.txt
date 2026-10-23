cmake_minimum_required(VERSION 3.16)
project(app03_controller LANGUAGES CXX)

add_executable(app03_controller main.cpp)
target_compile_features(app03_controller PRIVATE cxx_std_17)
target_compile_options(app03_controller PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(app03_controller PRIVATE
	APP03_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)