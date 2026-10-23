cmake_minimum_required(VERSION 3.16)
project(app01_sensor LANGUAGES CXX)

add_executable(app01_sensor main.cpp)
target_compile_features(app01_sensor PRIVATE cxx_std_17)
target_compile_options(app01_sensor PRIVATE -Wall -Wextra -Wpedantic)
