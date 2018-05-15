FROM centos:7

# This is a simple Dockerfile that will install lua_sandbox and hindsight. Often times you will also
# want the extensions, which are all the lua_sandbox plugins for use with hindsight. To get an image
# with those included, see the lua_sandbox_extensions repo:
#
# https://github.com/mozilla-services/lua_sandbox_extensions

WORKDIR /root

# Install most of our package dependencies here
RUN yum makecache && \
    yum install -y git rpm-build c-compiler make curl gcc gcc-c++ \
    autoconf automake centos-release-scl epel-release zlib-devel openssl-devel \
    libcurl-devel lua-devel && \
    yum install -y devtoolset-6 && \
    curl -OL https://cmake.org/files/v3.10/cmake-3.10.2-Linux-x86_64.tar.gz && \
    if [[ `sha256sum cmake-3.10.2-Linux-x86_64.tar.gz | awk '{print $1}'` != \
        "7a82b46c35f4e68a0807e8dc04e779dee3f36cd42c6387fd13b5c29fe62a69ea" ]]; then exit 1; fi && \
    (cd /usr && tar --strip-components=1 -zxf /root/cmake-3.10.2-Linux-x86_64.tar.gz) && \
    cat /etc/yum.conf | grep -v override_install_langs > /etc/yum.conf.lang && \
    cp /etc/yum.conf.lang /etc/yum.conf && \
    yum reinstall -y glibc-common

# Use devtoolset-6
ENV PERL5LIB='PERL5LIB=/opt/rh/devtoolset-6/root//usr/lib64/perl5/vendor_perl:/opt/rh/devtoolset-6/root/usr/lib/perl5:/opt/rh/devtoolset-6/root//usr/share/perl5/vendor_perl' \
    X_SCLS=devtoolset-6 \
    PCP_DIR=/opt/rh/devtoolset-6/root \
    LD_LIBRARY_PATH=/opt/rh/devtoolset-6/root/usr/lib64:/opt/rh/devtoolset-6/root/usr/lib \
    PATH=/opt/rh/devtoolset-6/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PYTHONPATH=/opt/rh/devtoolset-6/root/usr/lib64/python2.7/site-packages:/opt/rh/devtoolset-6/root/usr/lib/python2.7/site-packages \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

# Compile and install lua_sandbox using master branch
RUN git clone https://github.com/mozilla-services/lua_sandbox && \
    mkdir -p lua_sandbox/release && cd lua_sandbox/release && \
    cmake -DCMAKE_BUILD_TYPE=release .. && \
    make && ctest && cpack -G RPM && rpm -i *.rpm

# Compile and install hindsight
ADD . /root/hindsight
RUN mkdir -p /root/hindsight/release && cd /root/hindsight/release &&  \
	cmake -DCMAKE_BUILD_TYPE=release .. && \
	make && ctest && cpack -G RPM && rpm -i *.rpm

# Add a hindsight user and default RUN command
RUN groupadd hindsight && useradd -g hindsight -s /bin/bash -m hindsight
CMD /usr/bin/su - hindsight -c 'cd /home/hindsight && hindsight hindsight.cfg 7'
