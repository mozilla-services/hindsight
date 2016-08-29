# Hindsight

## Overview

Hindsight is a C based data processing infrastructure based on the [lua sandbox]
(https://github.com/mozilla-services/lua_sandbox) project.  I have received
several inquiries about a lighter weight and faster data pipeline with delivery
guarantees to replace [Heka](https://github.com/mozilla-services/heka).
Hindsight is that light weight skeleton around the same lua sandbox offering
'at least once' delivery semantics.

[Full Documentation](docs/index.md)

## Build

### Prerequisites

* Clang 3.1 or GCC 4.7+
* CMake (3.0+) - http://cmake.org/cmake/resources/software.html
* lua_sandbox (1.1+) - https://github.com/mozilla-services/lua_sandbox

### CMake Build Instructions

    git clone git@github.com:mozilla-services/hindsight.git
    cd hindsight 
    mkdir release
    cd release
    
    # Linux
    cmake -DCMAKE_BUILD_TYPE=release ..
    make
    ctest
    cpack -G TGZ # (DEB|RPM|ZIP)

    # Cross platform support is planned but not supported yet
