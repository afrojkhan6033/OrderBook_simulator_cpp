cmake_minimum_required(VERSION 3.15)
project(OrderBookSim)
add_compile_definitions(NOMINMAX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags for optimization
# set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
# set(CMAKE_CXX_FLAGS_DEBUG "-g -O2")

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

# Find Boost.Beast and Asio from vcpkg
find_package(boost_beast CONFIG REQUIRED)
find_package(boost_asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
# Live market data executable with WebSocket support
add_executable(LiveMarketData
        LiveMarketData.cpp
)

# Link libraries to LiveMarketData
find_package(Threads REQUIRED)

target_link_libraries(LiveMarketData
        PRIVATE
        Boost::beast
        Boost::asio
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        Threads::Threads
)

# Compiler-specific optimizations
if (MSVC)
    target_compile_options(LiveMarketData PRIVATE /O2 /arch:AVX2)
    target_compile_options(OrderBookTests PRIVATE /O2 /arch:AVX2)
else()
    target_compile_options(LiveMarketData PRIVATE -O3 -march=native -flto)
    target_compile_options(OrderBookTests PRIVATE -O3 -march=native)
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