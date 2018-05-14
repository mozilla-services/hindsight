# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.6 FATAL_ERROR)
project(hindsight VERSION 0.14.14 LANGUAGES C)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hindsight")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VENDOR        "Mozilla Services")
set(CPACK_PACKAGE_CONTACT       "Mike Trinkala <trink@mozilla.com>")
set(CPACK_STRIP_FILES           TRUE)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RPM_PACKAGE_LICENSE   "MPLv2.0")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "luasandbox (>= 1.2.3)")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "luasandbox-hindsight (>= 0.0.1)")
string(REGEX REPLACE "[()]" "" CPACK_RPM_PACKAGE_SUGGESTS ${CPACK_DEBIAN_PACKAGE_RECOMMENDS})
set(CPACK_DEBIAN_FILE_NAME      "DEB-DEFAULT")
set(CPACK_RPM_FILE_NAME         "RPM-DEFAULT")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(mozsvc)

find_package(luasandbox 1.2.3 REQUIRED CONFIG)
include(GNUInstallDirs)

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
  add_definitions(-D_POSIX_C_SOURCE=199506L -D_XOPEN_SOURCE=600)
endif()

install(PROGRAMS util/hindsight_timer_report.lua DESTINATION ${CMAKE_INSTALL_BINDIR})

add_subdirectory(src)

add_custom_target(docs COMMAND lua gen_gh_pages.lua
"${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}"
"${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
