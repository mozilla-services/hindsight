# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

include(ExternalProject)

set_property(DIRECTORY PROPERTY EP_BASE "${CMAKE_BINARY_DIR}/ep_base")

externalproject_add(
    "lua_sandbox"
    GIT_REPOSITORY https://github.com/mozilla-services/lua_sandbox.git
    GIT_TAG e9e8082d247b7d8b639094624a595c43da3a1f56
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_INSTALL_PREFIX=${PROJECT_PATH} -DLUA_JIT=off
    INSTALL_DIR ${PROJECT_PATH}
)

set(LUA_SANDBOX_LIBRARIES "${PROJECT_PATH}/lib/libluasandbox.so" )
