cmake_minimum_required(VERSION 3.16)
project(app03_controller LANGUAGES CXX)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core HttpServer Network)

set(CMAKE_AUTORCC ON)

add_executable(app03_controller main.cpp resources.qrc)
target_compile_features(app03_controller PRIVATE cxx_std_17)
target_compile_options(app03_controller PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(app03_controller PRIVATE
	Qt6::Core
	Qt6::HttpServer
	Qt6::Network
)
target_compile_definitions(app03_controller PRIVATE
	APP03_MAP_IMAGE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../app02-video-gen/build/test.png"
)