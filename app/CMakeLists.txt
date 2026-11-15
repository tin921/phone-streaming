cmake_minimum_required(VERSION 3.20)
project(ChurchPhoneStream LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Church Phone Stream is a Windows x64 program.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained .exe: the C++ runtime is linked in.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PS_INGEST_KEY "" CACHE STRING "Key built into the app (the server's INGEST_TOKEN)")
set(PS_DEFAULT_SERVER "wss://phone.example.org/ingest" CACHE STRING "Server address the app starts with")

add_executable(PhoneStream WIN32 src/main.cpp)
target_compile_definitions(PhoneStream PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    "PS_INGEST_KEY=\"${PS_INGEST_KEY}\"" "PS_DEFAULT_SERVER=\"${PS_DEFAULT_SERVER}\"")
target_link_libraries(PhoneStream PRIVATE ole32 winhttp crypt32 comctl32 shell32 user32 gdi32)
if(MSVC)
    target_compile_options(PhoneStream PRIVATE /W4 /utf-8 /permissive-)
    # Themed (common controls 6) buttons and lists.
    target_link_options(PhoneStream PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()
