# CMakeLists.txt - High-Performance FTP Client Library
# Phase 1: Foundation & C ABI Contract
#
# Minimum CMake version as per spec Section 9.2
cmake_minimum_required(VERSION 3.20)

project(ftpclient
    VERSION 1.0.0
    DESCRIPTION "High-Performance FTP Client Library with C ABI"
    LANGUAGES CXX C
)

# ============================================================================
# Build Configuration
# ============================================================================

# C++17 standard required, C++20 permitted (spec Section 9.2)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position Independent Code for shared library (spec Section 9.2)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# Compiler Flags and Symbol Visibility
# ============================================================================

# Symbol visibility control (spec Section 9.3)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/Clang: Hide all symbols by default, export only marked ones
    add_compile_options(-fvisibility=hidden)
    add_compile_options(-Wall -Wextra -Wpedantic)
    
    # Additional security hardening flags
    add_compile_options(-fstack-protector-strong)
    
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(MSVC)
    # MSVC: Use .def file for exports (preferred per spec Section 9.3)
    add_compile_options(/W4 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Od /Zi)
    endif()
endif()

# Define FTP_BUILDING_DLL when building the library
add_definitions(-DFTP_BUILDING_DLL)

# ============================================================================
# Source Files
# ============================================================================

set(FTP_SOURCES
    src/ftpclient.cpp
)

set(FTP_HEADERS
    include/ftpclient.h
)

set(FTP_INTERNAL_HEADERS
    src/FtpClientImpl.hpp
)

# Phase 2 Protocol Engine Headers (header-only implementation)
set(FTP_PROTOCOL_HEADERS
    src/protocol/Transport.hpp
    src/protocol/PlainTransport.hpp
    src/protocol/ErrorMap.hpp
    src/protocol/ReplyParser.hpp
    src/protocol/StateMachine.hpp
    src/protocol/DataChannel.hpp
    src/protocol/DirectoryWalker.hpp
    src/protocol/ControlThread.hpp
    src/protocol/ProtocolEngine.hpp
)

# Phase 3 Security Headers and Sources
set(FTP_SECURITY_HEADERS
    src/security/SecureAllocator.hpp
    src/security/CredentialVault.hpp
    src/security/TlsConfig.hpp
    src/security/TlsTransport.hpp
    src/security/OpenSSLInit.hpp
    src/security/SecretProvider.hpp
)

set(FTP_SECURITY_SOURCES
    src/security/CredentialVault.cpp
    src/security/TlsTransport.cpp
    src/security/OpenSSLInit.cpp
    src/security/SecretProvider.cpp
)

# ============================================================================
# Library Target
# ============================================================================

# Create shared library
add_library(ftpclient SHARED 
    ${FTP_SOURCES} 
    ${FTP_HEADERS} 
    ${FTP_INTERNAL_HEADERS}
    ${FTP_SECURITY_SOURCES}
)

# Set target properties
set_target_properties(ftpclient PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${FTP_HEADERS}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Include directories
target_include_directories(ftpclient
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    # Linux: Link pthread and OpenSSL
    target_link_libraries(ftpclient PRIVATE pthread ssl crypto)
elseif(APPLE)
    # macOS: Link OpenSSL (via Homebrew or system)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ftpclient PRIVATE OpenSSL::SSL OpenSSL::Crypto)
elseif(WIN32)
    # Windows: Link Winsock and OpenSSL
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ftpclient PRIVATE ws2_32 OpenSSL::SSL OpenSSL::Crypto)
endif()

# ============================================================================
# MSVC Module Definition File
# ============================================================================

if(MSVC)
    # Create .def file for explicit symbol exports
    set(DEF_FILE ${CMAKE_CURRENT_BINARY_DIR}/ftpclient.def)
    file(WRITE ${DEF_FILE}
        "EXPORTS\n"
        "    ftp_client_create\n"
        "    ftp_client_destroy\n"
        "    ftp_set_buffer_size\n"
        "    ftp_set_timeout_connect_ms\n"
        "    ftp_set_timeout_command_ms\n"
        "    ftp_connect\n"
        "    ftp_disconnect\n"
        "    ftp_ping\n"
        "    ftp_upload_dir\n"
        "    ftp_get_version\n"
        "    ftp_get_capabilities\n"
    )
    target_sources(ftpclient PRIVATE ${DEF_FILE})
endif()

# ============================================================================
# Test Executables
# ============================================================================

# ABI test in plain C (spec Section 12)
add_executable(abi_test tests/abi_test.c)
target_link_libraries(abi_test PRIVATE ftpclient)
set_target_properties(abi_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# Header purity test - compile header as pure C99
add_executable(header_purity_test tests/header_purity_test.c)
target_include_directories(header_purity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(header_purity_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    COMPILE_OPTIONS "-pedantic;-Wall;-Werror"
)

# Phase 2 Protocol Engine Tests
add_executable(phase2_test tests/phase2_test.cpp)
target_include_directories(phase2_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(phase2_test PRIVATE pthread)
set_target_properties(phase2_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ============================================================================
# Installation Rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS ftpclient
    EXPORT ftpclientTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT ftpclientTargets
    FILE ftpclientTargets.cmake
    NAMESPACE ftpclient::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ftpclient
)

# ============================================================================
# CMake Presets Compatibility
# ============================================================================

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Print configuration summary
message(STATUS "")
message(STATUS "=== FTP Client Library Configuration ===")
message(STATUS "Version:          ${PROJECT_VERSION}")
message(STATUS "Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:         ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "System:           ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Install Prefix:   ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
