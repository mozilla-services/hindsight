FROM centos:7

ADD . /app/src/hindsight/

RUN yum -y update && \
    yum -y install sudo && \

    # create the 'app' user
    groupadd -g 10001 app && \
    useradd -g app -G wheel -u 10001 -d /app app -s /bin/bash && \

    # allow app user to sudo without a password
    sed -i 's/^%wheel\tALL=(ALL)\tALL$/# %wheel\tALL=(ALL)\tALL/g' /etc/sudoers && \
    sed -i 's/^# %wheel\tALL=(ALL)\tNOPASSWD: ALL$/%wheel\tALL=(ALL)\tNOPASSWD: ALL/g' /etc/sudoers && \
    chown app:app /app -R

USER app
WORKDIR /app

RUN sudo yum -y install https://dl.fedoraproject.org/pub/epel/7/x86_64/e/epel-release-7-9.noarch.rpm && \
    sudo yum -y install lua-devel luarocks cmake3 make clang gcc git rpm-build sudo && \
    sudo ln -s /usr/bin/cmake3 /usr/local/bin/cmake && \

    # Install confluent 3.1 for centos 7 for librdkafka-devel
    echo [confluent] > confluent.repo && \
    echo name=confluent >> confluent.repo && \
    echo baseurl=http://packages.confluent.io/rpm/3.1/7 >> confluent.repo && \
    echo gpgcheck=1 >> confluent.repo && \
    echo gpgkey=http://packages.confluent.io/rpm/3.1/archive.key >> confluent.repo && \
    sudo mv confluent.repo /etc/yum.repos.d && \

    # Build the lua sandbox & extensions
    cd /app/src && \
    git clone https://github.com/mozilla-services/lua_sandbox.git && \
    git clone https://github.com/mozilla-services/lua_sandbox_extensions.git && \
    cd lua_sandbox_extensions/ && \
    . /app/src/lua_sandbox/build/functions.sh && \
    build_lsbe() { \
        sudo yum -y install https://net-mozaws-prod-ops-rpmrepo-deps.s3.amazonaws.com/hindsight/parquet-cpp-0.0.1-Linux.rpm && \
        install_packages c++-compiler librdkafka-devel openssl-devel postgresql-devel systemd-devel zlib-devel && \
        rm -rf ./release && \
        mkdir release && \
        cd release && \
        cmake -DCMAKE_BUILD_TYPE=release -DENABLE_ALL_EXT=true -DEXT_geoip=false -DEXT_snappy=false "-DCPACK_GENERATOR=${CPACK_GENERATOR}" .. && \
        make && \
        ctest -V && \
        make packages; \
    } && \
    build_function="build_lsbe" main && \
    sudo rpm -ivh /app/src/lua_sandbox_extensions/release/luasandbox*Linux.rpm && \

    # Build hindsight
    cd /app/src/hindsight && \
    mkdir release && \
    cd release && \
    cmake3 -DCMAKE_BUILD_TYPE=release .. && \
    make && \
    ctest3 && \
    cpack3 -G RPM && \
    sudo rpm -ivh /app/src/hindsight/release/hindsight*Linux.rpm && \

    # Setup run directory
    cd /app && \
    mkdir -p /app/cfg \
             /app/input \
             /app/output/input \
             /app/load \
             /app/run/input \
             /app/run/analysis \
             /app/run/output && \
    cp /app/src/hindsight/hindsight.cfg /app/cfg/hindsight.cfg && \

    # some extra lua libraries
    sudo luarocks install lrexlib-pcre && \

    # cleanup
    rm -rf /app/src && \
    sudo yum -y remove cmake3 make clang git rpm-build && \
    sudo yum -y autoremove && \
    sudo yum -y clean all

VOLUME /app/output /app/load /app/run /app/input

CMD /usr/bin/hindsight /app/cfg/hindsight.cfg
