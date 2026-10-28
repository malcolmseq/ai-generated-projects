cmake_minimum_required(VERSION 3.16)
project(app01_sensor LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

add_executable(app01_sensor main.cpp)
target_compile_features(app01_sensor PRIVATE cxx_std_17)
target_compile_options(app01_sensor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(app01_sensor PRIVATE
	Qt${QT_VERSION_MAJOR}::Core
	Qt${QT_VERSION_MAJOR}::Network
)
