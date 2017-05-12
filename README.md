# Hindsight

## Overview

Hindsight is a C based data processing infrastructure based on the
[lua sandbox](https://github.com/mozilla-services/lua_sandbox) project.  I have
received several inquiries about a lighter weight and faster data pipeline with
delivery guarantees to replace [Heka](https://github.com/mozilla-services/heka).
Hindsight is that light weight skeleton around the same lua sandbox offering
'at least once' delivery semantics.

* [Full Documentation](http://mozilla-services.github.io/hindsight)
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

## Releases

* The master branch is the current release and is considered stable at all
  times.
* New versions can be released as frequently as every two weeks (our sprint
  cycle). The only exception would be for a high priority patch.
* All active work is flagged with the sprint milestone and tracked in the
  project dashboard.
* New releases occur the day after the sprint finishes.
  * The version in the dev branch is updated
  * The changes are merged into master
  * A new tag is created

## Contributions

* All pull requests must be made against the dev branch, direct commits to
  master are not permitted.
* All non trivial contributions should start with an issue being filed (if it is
  a new feature please propose your design/approach before doing any work as not
  all feature requests are accepted).
