cmake_minimum_required(VERSION 3.14)
project(cpp_gemini_backend)
set(CMAKE_CXX_STANDARD 17)

# Require cpr for HTTP client and nlohmann_json for JSON parsing
find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)

# Include Crow single-header (place crow_all.h into include/)
include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(backend main.cpp)

target_link_libraries(backend
    PRIVATE
        cpr::cpr
        nlohmann_json::nlohmann_json
)