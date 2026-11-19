cmake_minimum_required(VERSION 3.15)
project(HareParseJson LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hareparse_json STATIC json.cpp)
target_include_directories(hareparse_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(hareparse_json PRIVATE /W4 /utf-8)
else()
  target_compile_options(hareparse_json PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(json-cli main.cpp)
target_link_libraries(json-cli PRIVATE hareparse_json)

add_executable(model-demo examples/model_demo.cpp)
target_link_libraries(model-demo PRIVATE hareparse_json)

enable_testing()
add_executable(json-tests test_json.cpp)
target_link_libraries(json-tests PRIVATE hareparse_json)
add_test(NAME json-tests COMMAND json-tests)
