cmake_minimum_required(VERSION 3.14)
project(dnd_api_backend VERSION 1.0 LANGUAGES CXX)

# ————————————————————————————————————————————————
# 1) Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ————————————————————————————————————————————————
# 2) FetchContent for header-only Crow, CPR, and nlohmann/json
include(FetchContent)

# Crow (header-only web framework)
FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG        v1.2.1.2
)
FetchContent_MakeAvailable(crow)

# CPR (HTTP client)
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/libcpr/cpr.git
  GIT_TAG        1.10.0
)
FetchContent_MakeAvailable(cpr)

# nlohmann/json (JSON for Modern C++)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

# ————————————————————————————————————————————————
# 3) Find system libraries
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ————————————————————————————————————————————————
# 4) Define the backend executable
add_executable(backend main.cpp)

# ————————————————————————————————————————————————
# 5) Link against all needed targets
target_link_libraries(backend
  PRIVATE
    Crow::Crow                 # Crow’s INTERFACE library target :contentReference[oaicite:1]{index=1}
    cpr::cpr                   # from CPR’s FetchContent
    nlohmann_json::nlohmann_json  # from nlohmann/json’s FetchContent
    OpenSSL::SSL               # for HTTPS support
    OpenSSL::Crypto
    Threads::Threads           # pthreads (used by Asio under the hood)
)