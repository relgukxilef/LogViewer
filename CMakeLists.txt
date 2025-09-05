cmake_minimum_required(VERSION 3.22) #act only supports 3.22
project(LogViewer LANGUAGES CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG release-1.4.4
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL TRUE
)
FetchContent_Declare(
    Boost
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG boost-1.89.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL TRUE
)
FetchContent_MakeAvailable(fltk Boost)

add_executable(LogViewer source/main.cpp)
target_link_libraries(LogViewer fltk Boost::asio)

add_custom_target(
    test_data LogViewer --make_test_data big.csv
    DEPENDS LogViewer
    BYPRODUCTS big.csv
)
