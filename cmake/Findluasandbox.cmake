# - Find luasandbox
# Find the native LUASANDBOX headers and libraries.
#
#  LUASANDBOX_INCLUDE_DIRS - where to find luasandbox.h, etc.
#  LUASANDBOX_LIBRARIES    - List of libraries when using luasandbox.
#  LUASANDBOX_MODULES      - Path to the luasandbox modules.
#  LUASANDBOX_FOUND        - True if luasandbox found.

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Look for the header file.
FIND_PATH(LUASANDBOX_INCLUDE_DIR NAMES luasandbox.h PATHS "/usr/local/include" "/usr/include")
MARK_AS_ADVANCED(LUASANDBOX_INCLUDE_DIR)

# Look for the sandbox library.
FIND_LIBRARY(LUASANDBOX_LIBRARY NAMES
    luasandbox
    PATHS "/usr/local/lib" "/usr/lib"
)
MARK_AS_ADVANCED(LUASANDBOX_LIBRARY)

# Look for the Lua library.
FIND_LIBRARY(LUASB_LIBRARY NAMES
    luasb
    PATHS "/usr/local/lib" "/usr/lib"
)
MARK_AS_ADVANCED(LUASB_LIBRARY)

# Look for the sandbox modules.
FIND_PATH(LUASANDBOX_MODULES NAMES
    modules
    PATHS "/usr/local/lib/luasandbox" "/usr/lib/luasandbox"
)
MARK_AS_ADVANCED(LUASANDBOX_MODULES)

# handle the QUIETLY and REQUIRED arguments and set LUASANDBOX_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LUASANDBOX DEFAULT_MSG LUASANDBOX_LIBRARY LUASB_LIBRARY LUASANDBOX_MODULES LUASANDBOX_INCLUDE_DIR)

IF(LUASANDBOX_FOUND)
  SET(LUASANDBOX_LIBRARIES ${LUASANDBOX_LIBRARY} ${LUASB_LIBRARY})
  SET(LUASANDBOX_INCLUDE_DIRS ${LUASANDBOX_INCLUDE_DIR})
ENDIF(LUASANDBOX_FOUND)
