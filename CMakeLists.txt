cmake_minimum_required(VERSION 3.28)


if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_TOOLCHAIN_FILE
            "C:/Users/username/Downloads/vcpkg-master/vcpkg-master/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain" FORCE)
endif()


project(WASM2JS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)         


include(FetchContent)

FetchContent_Declare(
        wabt
        GIT_REPOSITORY https://github.com/WebAssembly/wabt.git
        GIT_TAG        1.0.37)

FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3)



FetchContent_Declare(
        cpp-httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.15.3)

FetchContent_MakeAvailable(wabt json cpp-httplib)
add_compile_definitions(
        _CRT_MEMORY_DEFINED       
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        NOGDI
        NOCRYPT
)

if(TARGET wabt)
    target_compile_definitions(wabt PRIVATE
            _CRT_MEMORY_DEFINED WIN32_LEAN_AND_MEAN NOMINMAX NOGDI NOCRYPT)
endif()

if(NOT HTTP_ONLY)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(OPENSSL_ROOT_DIR "C:/msys64/mingw64" CACHE PATH "")
        set(OPENSSL_USE_STATIC_LIBS ON)      
    endif()

    find_package(OpenSSL REQUIRED)
endif()


add_executable(WASM2JS
        main.cpp
        Parser.cpp  Parser.h
        Utils.cpp   Utils.h)

target_include_directories(WASM2JS PRIVATE
        ${wabt_SOURCE_DIR}/include
        ${json_SOURCE_DIR}/include         
        ${cpp-httplib_SOURCE_DIR}          
)

target_link_libraries(WASM2JS PRIVATE
        wabt
        httplib::httplib)

if(NOT HTTP_ONLY)
    target_link_libraries(WASM2JS PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()


if(MSVC)
    target_compile_options(WASM2JS PRIVATE /EHa)
    target_link_options  (WASM2JS PRIVATE /STACK:134217728)
else()  
    target_link_options  (WASM2JS PRIVATE -Wl,--stack,134217728)
endif()
