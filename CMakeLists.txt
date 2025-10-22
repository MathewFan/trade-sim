cmake_minimum_required(VERSION 3.20)
project(tradesim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_executable(ws_tradesim apps/ws_server.cpp engine/engine.cpp)
target_include_directories(ws_tradesim PRIVATE engine)
