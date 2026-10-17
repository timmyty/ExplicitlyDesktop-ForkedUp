# ExplicitlyTests: unit tests for the portable phone-streaming code (no JUCE,
# no audio device). Builds on its own or from the top-level CMakeLists.txt:
#   cmake -S tests/cpp -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
# or, from the app build:  cmake --build build --target ExplicitlyTests
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ExplicitlyTests CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()

set(EXPLICITLY_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source)

add_executable(ExplicitlyTests
    TestMain.cpp
    CryptoTests.cpp
    ProtocolTests.cpp
    PcmTapTests.cpp
    RemoteSessionsTests.cpp
    ${EXPLICITLY_SOURCE_DIR}/StreamCrypto.cpp
    ${EXPLICITLY_SOURCE_DIR}/PcmTap.cpp
    ${EXPLICITLY_SOURCE_DIR}/RemoteSessions.cpp
)

target_include_directories(ExplicitlyTests PRIVATE ${EXPLICITLY_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ExplicitlyTests PRIVATE
    EXPLICITLY_VECTORS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../vectors/protocol_vectors.json")

find_package(Threads REQUIRED)
target_link_libraries(ExplicitlyTests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(ExplicitlyTests PRIVATE bcrypt)
endif()

if(MSVC)
    target_compile_options(ExplicitlyTests PRIVATE /W4 /utf-8)
else()
    target_compile_options(ExplicitlyTests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ExplicitlyTests COMMAND ExplicitlyTests)
