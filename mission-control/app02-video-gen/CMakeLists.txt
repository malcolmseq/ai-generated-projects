cmake_minimum_required(VERSION 3.16)
project(app02_video_gen LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network)

set(CMAKE_AUTOMOC ON)

add_executable(app02_video_gen main.cpp video_generator.cpp)
target_compile_features(app02_video_gen PRIVATE cxx_std_17)
target_compile_options(app02_video_gen PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(app02_video_gen PRIVATE
	Threads::Threads
	Qt6::Core
	Qt6::Gui
	Qt6::Network
)
