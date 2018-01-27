#!/bin/sh

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

set -e

hindsight_dir="$PWD"
lsb_dir="$(dirname "$hindsight_dir")/lua_sandbox"

if [ ! -d "$lsb_dir" ]; then
    (   set -x;
        git clone https://github.com/mozilla-services/lua_sandbox "$lsb_dir"
        git rev-parse HEAD
    )
fi

lse_dir="$(dirname "$hindsight_dir")/lua_sandbox_extensions"
if [ ! -d "$lse_dir" ]; then
    (   set -x;
        git clone https://github.com/mozilla-services/lua_sandbox_extensions "$lse_dir"
    )
fi

. "$lsb_dir/build/functions.sh"

lse_build() {
    (   set -x

        rm -rf ./release
        mkdir release
        cd release

        cmake -DCMAKE_BUILD_TYPE=release -DEXT_heka=on -DEXT_lpeg=on \
              -DEXT_socket=on -DEXT_cjson=on \
              "-DCPACK_GENERATOR=${CPACK_GENERATOR}" ..
        make
        make packages
    )
}

setup_env
install_packages c-compiler cmake make rpm-build
old_dir="$PWD"
echo "+cd $lsb_dir"
cd "$lsb_dir"
build
install_packages_from_dir ./release
echo "+cd $lse_dir"
cd "$lse_dir"
lse_build
install_packages_from_dir ./release
echo "+cd $old_dir"
cd "$old_dir"
build
install_packages_from_dir ./release
cd release
ctest -C hindsight -VV

