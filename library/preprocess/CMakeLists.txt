cmake_minimum_required(VERSION 3.15)
project(HareParsePreprocess LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hareparse_preprocess STATIC preprocess.cpp)
target_include_directories(hareparse_preprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(hareparse_preprocess PRIVATE /W4 /utf-8)
else()
  target_compile_options(hareparse_preprocess PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(preprocess-cli main.cpp)
target_link_libraries(preprocess-cli PRIVATE hareparse_preprocess)

enable_testing()
add_executable(preprocess-tests test_preprocess.cpp)
target_link_libraries(preprocess-tests PRIVATE hareparse_preprocess)
add_test(NAME preprocess-tests
         COMMAND preprocess-tests ${CMAKE_CURRENT_SOURCE_DIR}/examples)
