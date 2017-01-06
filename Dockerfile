FROM centos:7

RUN groupadd -g 10001 app && \
    useradd -g app -u 10001 -m -d /app app -s /bin/bash

RUN yum -y update && \
    yum -y install wget && \
    wget https://dl.fedoraproject.org/pub/epel/7/x86_64/e/epel-release-7-8.noarch.rpm && \
    rpm -ivh epel-release-7-8.noarch.rpm && \
    yum -y install cmake3 make clang gcc git rpm-build

# Build the lua sandbox
RUN cd /app && \
    git clone https://github.com/mozilla-services/lua_sandbox.git && \
    cd lua_sandbox/ && \
    mkdir release && \
    cd release && \
    cmake3 -DCMAKE_BUILD_TYPE=release .. && \
    make && \
    #ctest3 && \
    cpack3 -G RPM && \
    rpm -ivh /app/lua_sandbox/release/luasandbox*Linux.rpm

# Build hindsight
RUN mkdir /app/hindsight
ADD . /app/hindsight/
RUN cd /app/hindsight && \
    mkdir release && \
    cd release && \
    cmake3 -DCMAKE_BUILD_TYPE=release .. && \
    make && \
    ctest3 && \
    cpack3 -G RPM && \
    rpm -ivh /app/hindsight/release/hindsight*Linux.rpm

# Configuration
USER app
RUN mkdir -p /app/output \
            /app/load \
            /app/run/input \
            /app/run/analysis \
            /app/run/output \
            /app/sandboxes/heka \
            /app/modules \
            /app/io_modules
RUN cp /app/hindsight/hindsight.cfg /app/hindsight.cfg

WORKDIR /app
CMD /usr/bin/hindsight /app/hindsight.cfg
