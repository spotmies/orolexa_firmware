# Install script for directory: /Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/Users/tejasai/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/build_info.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/debug.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/error.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/net_sockets.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/oid.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/pkcs7.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/ssl.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/ssl_cache.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/ssl_ciphersuites.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/ssl_cookie.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/ssl_ticket.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/timing.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/version.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/x509.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/x509_crl.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/x509_crt.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/x509_csr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls/private" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/private/config_adjust_ssl.h"
    "/Users/tejasai/Downloads/orolexa/DentalS3/.espressif/esp-idf/components/mbedtls/mbedtls/include/mbedtls/private/config_adjust_x509.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/tejasai/Downloads/orolexa/DentalS3/build/esp-idf/mbedtls/mbedtls/include/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
