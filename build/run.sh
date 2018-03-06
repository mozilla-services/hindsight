#!/bin/sh

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Author: Mathieu Parent <math.parent@gmail.com>

set -e

root_dir="$PWD"
lsb_dir="$(dirname "$root_dir")/lua_sandbox"
hs_dir="$(dirname "$root_dir")/hindsight"

if [ ! -d "$lsb_dir" ]; then
    (   set -x;
        git clone https://github.com/mozilla-services/lua_sandbox "$lsb_dir"
        git rev-parse HEAD
    )
fi

. "$lsb_dir/build/functions.sh"

if [ "$1" != "build" -o $# -ge 2 ]; then
    usage
    exit 1
fi

build_hindsight() {
    echo "+cd $hs_dir"
    cd "$hs_dir"
    build
    if [ "$DISTRO" = "fedora:latest" ]; then
        (   set -x;
            $as_root yum install -y lua
        )
    fi
    install_packages_from_dir ./release
}

build_function="build_hindsight"
main
