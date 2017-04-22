# Hindsight

## Overview

Hindsight is a C-based data processing infrastructure based on the [lua sandbox](https://github.com/mozilla-services/lua_sandbox) project. I have received several inquiries about a lighter weight and faster data pipeline with delivery guarantees to replace [Heka](https://github.com/mozilla-services/heka). Hindsight is that lightweight skeleton around the same Lua sandbox offering 'at least once' delivery semantics.

* [Full Documentation](docs/index.md)
* Support
    * IRC: [#hindsight on irc.mozilla.org](irc://irc.mozilla.org/hindsight)
    * Mailing list: https://mail.mozilla.org/listinfo/hindsight

## Build

### Prerequisites

* Clang 3.1 or GCC 4.7+
* CMake (3.0+) - http://cmake.org/cmake/resources/software.html
* lua_sandbox (1.1+) - https://github.com/mozilla-services/lua_sandbox

### CMake Build Instructions

    git clone https://github.com/mozilla-services/hindsight.git
    cd hindsight 
    mkdir release
    cd release
    
    # Linux
    cmake -DCMAKE_BUILD_TYPE=release ..
    make
    ctest
    cpack -G TGZ # (DEB|RPM|ZIP)

    # Cross platform support is planned but not supported yet
