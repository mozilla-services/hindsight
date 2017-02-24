# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(hindsight VERSION 0.13.0 LANGUAGES C)
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
  add_definitions(-D_POSIX_C_SOURCE=199309L -D_XOPEN_SOURCE=600)
endif()

set(INSTALL_SANDBOX_PATH ${CMAKE_INSTALL_DATAROOTDIR}/luasandbox/sandboxes)
set(DPERMISSION DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(EMPTY_DIR ${CMAKE_BINARY_DIR}/empty)
file(MAKE_DIRECTORY ${EMPTY_DIR})
install(DIRECTORY ${EMPTY_DIR}/ DESTINATION ${INSTALL_SANDBOX_PATH} ${DPERMISSION})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sandboxes/ DESTINATION ${INSTALL_SANDBOX_PATH}/heka ${DPERMISSION})

add_subdirectory(src)
