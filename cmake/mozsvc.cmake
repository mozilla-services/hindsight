# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

if(MSVC)
    # Predefined Macros: http://msdn.microsoft.com/en-us/library/b0084kay.aspx
    # Compiler options: http://msdn.microsoft.com/en-us/library/fwkeyyhe.aspx

    # set a high warning level and treat them as errors
    set(CMAKE_C_FLAGS           "/W3 /WX")

    # debug multi threaded dll runtime, complete debugging info, runtime error checking
    set(CMAKE_C_FLAGS_DEBUG     "/MDd /Zi /RTC1")

    # multi threaded dll runtime, optimize for speed, auto inlining
    set(CMAKE_C_FLAGS_RELEASE   "/MD /O2 /Ob2")

    set(CPACK_GENERATOR         "NSIS")
else()
    # Predefined Macros: clang|gcc -dM -E -x c /dev/null
    # Compiler options: http://gcc.gnu.org/onlinedocs/gcc/Invoking-GCC.html#Invoking-GCC
    set(CMAKE_C_FLAGS   "-std=c99 -pedantic -Werror -Wno-error=deprecated -Wall -Wextra -fPIC")
    set(CMAKE_C_FLAGS_DEBUG     "-g")

    set(CMAKE_C_FLAGS_RELEASE   "-O2")

    set(CMAKE_C_FLAGS_PROFILE   "${CMAKE_C_FLAGS_RELEASE} -g -pg")

    set(CPACK_GENERATOR         "TGZ")

    set(CMAKE_SKIP_BUILD_RPATH              FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH      FALSE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH   FALSE)
endif()

include(CPack)
include(CTest)
