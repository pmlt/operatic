cmake_minimum_required(VERSION 3.10)

# set the project name
project(operatic VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pedantic -Wextra -Werror")

# add opl3 library
add_library(dbopl dbopl.cpp)

# add the executable
add_executable(operatic operatic.cpp)
target_link_libraries(operatic PUBLIC dbopl pulse pulse-simple)
