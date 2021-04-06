# Hindsight

## Overview

Hindsight is a C based data processing infrastructure based on the
[lua sandbox](https://github.com/mozilla-services/lua_sandbox) project.  I have
received several inquiries about a lighter weight and faster data pipeline with
delivery guarantees to replace [Heka](https://github.com/mozilla-services/heka).
Hindsight is that light weight skeleton around the same lua sandbox offering
'at least once' delivery semantics. The skeleton is supplemented by
[extension packages](https://mozilla-services.github.io/lua_sandbox_extensions)
including hundreds of data structures, algorithms, plugins, parsers and
grammars. The extensions repository is where most of the active development is
happening now as the core infrastructure (Hindsight and the [Lua Sandbox](https://github.com/mozilla-services/lua_sandbox))
is stable and changes infrequently.  There is also a [Hindsight Administration UI](https://github.com/mozilla-services/hindsight_admin)
available for monitoring, debugging and plugin management (you can check out a
running instance here: [hsadmin](https://hsadmin.trink.com/))

* [Full Documentation](http://mozilla-services.github.io/hindsight)
* Support
    * Chat: [Matrix](https://chat.mozilla.org/#/room/#hindsight:mozilla.org)
    * Mailing list: https://mail.mozilla.org/listinfo/hindsight

## Build

### Prerequisites

* Clang 3.1 or GCC 4.7+
* CMake (3.6+) - http://cmake.org/cmake/resources/software.html
* lua_sandbox (1.2.3+) - https://github.com/mozilla-services/lua_sandbox
* OpenSSL (1.0.x+, optional)

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

By default hindsight is linked against OpenSSL and configured to set locking callbacks in the
library to ensure proper threaded operation. If this functionality is not desired the cmake
build option `-DWITHOUT_OPENSSL=true` can be used to disable this, for example if you are not
using any sandboxes/modules that make use of OpenSSL and do not want the dependency.

## Releases

* The main branch is the current release and is considered stable at all
  times.
* New versions can be released as frequently as every two weeks (our sprint
  cycle). The only exception would be for a high priority patch.
* All active work is flagged with the sprint milestone and tracked in the
  project dashboard.
* New releases occur the day after the sprint finishes.
  * The version in the dev branch is updated
  * The changes are merged into main
  * A new tag is created

## Docker Images

[Docker images](https://hub.docker.com/r/mozilla/hindsight/tags) are constructed from the
main and dev branches and can be pulled, or built using the Dockerfile.

Note that the Docker image built here is only a bare bones image containing just lua_sandbox
and hindsight. For a more full featured image that also contains all of the extensions, see
the Docker image for the [extensions](https://github.com/mozilla-services/lua_sandbox_extensions)
repo.

## Contributions

* All pull requests must be made against the dev branch, direct commits to
  main are not permitted.
* All non trivial contributions should start with an issue being filed (if it is
  a new feature please propose your design/approach before doing any work as not
  all feature requests are accepted).
