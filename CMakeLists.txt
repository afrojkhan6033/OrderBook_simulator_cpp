cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0167 NEW)
project(OrderBookSim)
add_compile_definitions(NOMINMAX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags for optimization
if(MSVC)
    # Link-Time Optimization (LTO) for Release builds
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /DNDEBUG")
    # Profile-Guided Optimization (PGO) - uncomment to use:
    # set(CMAKE_CXX_FLAGS_PGO "/O2 /GL /Gy /DNDEBUG")
else()
    # GCC/Clang LTO and optimization
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto -march=native")
    # PGO instrumentation (first pass):
    # set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fprofile-generate -march=native")
    # PGO optimization (second pass, after training):
    # set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fprofile-use -march=native")
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-g -O2")

# Enable AVX2 for SIMD optimizations
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:AVX2")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mfma")
endif()

# Header files
set(HEADERS
        OrderType.h
        Types.h
        LevelInfo.h
        Order.h
        OrderModify.h
        Trade.h
        OrderBook.h
        Constants.h
        MarketDataFeed.h
        ExchangeRules.h
)

# Test executable (functionality and performance tests)
add_executable(OrderBookTests
        tests.cpp
)

# Fetch dependencies
include(FetchContent)

# Fetch nlohmann_json
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# # Fetch CURL with SSL support for WebSocket
# FetchContent_Declare(
#         curl
#         GIT_REPOSITORY https://github.com/curl/curl.git
#         GIT_TAG curl-8_4_0
# )

# # Configure CURL options for WebSocket
# set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# set(CURL_STATICLIB ON CACHE BOOL "" FORCE)
# set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
# set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# set(HTTP_ONLY OFF CACHE BOOL "" FORCE)  # Need this for WebSocket
# set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
# set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
# set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
# set(CMAKE_USE_SCHANNEL ON CACHE BOOL "" FORCE)

# FetchContent_MakeAvailable(curl)

# Find libraries from vcpkg
set(BOOST_ROOT "A:/vcpkg/installed/x64-windows")
include_directories(${BOOST_ROOT}/include)

set(OPENSSL_ROOT_DIR "A:/vcpkg/installed/x64-windows")
set(OPENSSL_USE_STATIC_LIBS OFF)
find_package(OpenSSL REQUIRED)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Live market data executable with WebSocket support
add_executable(LiveMarketData
        LiveMarketData.cpp
)

# Link libraries to LiveMarketData
target_link_libraries(LiveMarketData
        PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        nlohmann_json::nlohmann_json
        Threads::Threads
)

# Include directories for Boost (header-only)
target_include_directories(LiveMarketData PRIVATE ${BOOST_ROOT}/include)

# ImGui renderer - deferred pending header compatibility fixes
# add_executable(ImGuiRenderer ImGuiRenderer.cpp)

# Compiler-specific optimizations
if (MSVC)
    target_compile_options(LiveMarketData PRIVATE /arch:AVX2 /GL /Gy)
    target_compile_options(OrderBookTests PRIVATE /arch:AVX2 /GL /Gy)
    # Enable link-time code generation
    set_target_properties(LiveMarketData PROPERTIES LINK_FLAGS "/LTCG /OPT:REF /OPT:ICF")
    set_target_properties(OrderBookTests PROPERTIES LINK_FLAGS "/LTCG /OPT:REF /OPT:ICF")
else()
    target_compile_options(LiveMarketData PRIVATE -O3 -march=native -flto -ffast-math -fno-exceptions -fno-rtti)
    target_compile_options(OrderBookTests PRIVATE -O3 -march=native -ffast-math -fno-exceptions)
endif()

# Enable testing
enable_testing()
add_test(NAME OrderBookTests COMMAND OrderBookTests)

# Print build information
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "WebSocket Support: Enabled")

# Find clang-format
find_program(CLANG_FORMAT "clang-format")

if (CLANG_FORMAT)
    # Get all source files
    file(GLOB_RECURSE ALL_SOURCE_FILES *.cpp *.h)

    # Add custom target for formatting
    add_custom_target(
            format
            COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCE_FILES}
            COMMENT "Running clang-format"
    )
endif ()