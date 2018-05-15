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
RUN sudo yum -y install centos-release-scl-rh
RUN sudo yum -y install devtoolset-3-gcc devtoolset-3-gcc-c++
RUN sudo update-alternatives --install /usr/bin/gcc-4.9 gcc-4.9 /opt/rh/devtoolset-3/root/usr/bin/gcc 10
RUN sudo update-alternatives --install /usr/bin/g++-4.9 g++-4.9 /opt/rh/devtoolset-3/root/usr/bin/g++ 10
RUN sudo rm -f /usr/bin/g++ && sudo rm -f /usb/bin/gcc
RUN sudo ln -s /usr/bin/g++-4.9 /usr/bin/g++
RUN sudo ln -s /usr/bin/gcc-4.9 /usr/bin/gcc
RUN sudo yum -y install epel-release.noarch && \
    sudo yum -y install lua-devel luarocks cmake3 make git rpm-build sudo && \
    sudo ln -s /usr/bin/cmake3 /usr/local/bin/cmake

    # Install confluent 3.1 for centos 7 for librdkafka-devel
RUN     echo -e "[confluent]\n\
name=confluent\n\
baseurl=http://packages.confluent.io/rpm/3.1/7\n\
gpgcheck=1\n\
gpgkey=http://packages.confluent.io/rpm/3.1/archive.key\n" | sudo tee /etc/yum.repos.d/confluent.repo && \

    # Build the lua sandbox & extensions
    cd /app/src && \
    git clone https://github.com/mozilla-services/lua_sandbox.git && \
    git clone https://github.com/mozilla-services/lua_sandbox_extensions.git && \
    cd lua_sandbox_extensions/ && \
    . /app/src/lua_sandbox/build/functions.sh && \
    build_lsbe() { \
        install_packages  librdkafka-devel openssl-devel postgresql-devel systemd-devel zlib-devel && \
        rm -rf ./release && \
        mkdir release && \
        cd release && \
        cmake -DCMAKE_BUILD_TYPE=release \
        -DEXT_aws=off \
        -DEXT_bloom_filter=on \
        -DEXT_circular_buffer=on \
        -DEXT_cjson=on \
        -DEXT_compat=on \
        -DEXT_cuckoo_filter=on \
        -DEXT_elasticsearch=on \
        -DEXT_geoip=off \
        -DEXT_heka=on \
        -DEXT_hyperloglog=on \
        -DEXT_jose=off \
        -DEXT_kafka=on \
        -DEXT_lfs=on \
        -DEXT_lpeg=on \
        -DEXT_lsb=on \
        -DEXT_moz_ingest=on \
        -DEXT_moz_logging=on \
        -DEXT_moz_pioneer=off \
        -DEXT_moz_security=off \
        -DEXT_moz_telemetry=on \
        -DEXT_openssl=on \
        -DEXT_parquet=off \
        -DEXT_postgres=on \
        -DEXT_rjson=on \
        -DEXT_sax=on \
        -DEXT_snappy=off \
        -DEXT_socket=on \
        -DEXT_ssl=on \
        -DEXT_struct=on \
        -DEXT_syslog=on \
        -DEXT_systemd=on \
        -DEXT_zlib=on \
	"-DCPACK_GENERATOR=${CPACK_GENERATOR}" .. && \
        make && \
        ctest -V && \
        make packages; \
    } && \
    build_function="build_lsbe" main && \
    sudo yum install -y /app/src/lua_sandbox_extensions/release/luasandbox*.rpm && \

    # Build hindsight
    cd /app/src/hindsight && \
    mkdir release && \
    cd release && \
    cmake3 -DCMAKE_BUILD_TYPE=release .. && \
    make && \
    ctest3 && \
    cpack3 -G RPM && \
    sudo yum install -y /app/src/hindsight/release/hindsight*.rpm && \

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
    sudo yum -y remove cmake3 make clang git rpm-build c++-compiler librdkafka-devel openssl-devel postgresql-devel systemd-devel zlib-devel  && \
    sudo yum -y autoremove && \
    sudo yum -y clean all

VOLUME /app/output /app/load /app/run /app/input

CMD /usr/bin/hindsight /app/cfg/hindsight.cfg
