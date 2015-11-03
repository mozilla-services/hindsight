# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(hindsight)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hindsight")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 3)
set(CPACK_PACKAGE_VERSION_PATCH 0)

set(PROJECT_PATH "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(trink)
find_package(luasandbox REQUIRED)

if(CMAKE_HOST_UNIX)
  find_library(PTHREAD_LIBRARY pthread)
  find_library(LIBDL_LIBRARY dl)
  find_library(LIBM_LIBRARY m)
  find_library(LIBRT_LIBRARY rt)

  set(UNIX_LIBRARIES
  ${PTHREAD_LIBRARY}
  ${LIBDL_LIBRARY}
  ${LIBM_LIBRARY}
  ${LIBRT_LIBRARY})
  add_definitions(-D_POSIX_C_SOURCE=199309L -D_XOPEN_SOURCE=600)
endif()

include_directories(${PROJECT_PATH}/include)

add_subdirectory(src)

