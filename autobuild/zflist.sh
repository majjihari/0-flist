#!/bin/bash
set -ex

makeopts="-j 12"

dependencies() {
    apt-get update

    apt-get install -y build-essential git libsnappy-dev libz-dev \
        libtar-dev libb2-dev autoconf libtool libjansson-dev \
        libhiredis-dev libsqlite3-dev libssl-dev
}

libcurl() {
    git clone --depth=1 -b curl-7_62_0 https://github.com/curl/curl
    pushd curl
    autoreconf -f -i -s

    ./configure --disable-debug --enable-optimize --disable-curldebug --disable-symbol-hiding --disable-rt \
        --disable-ftp --disable-ldap --disable-ldaps --disable-rtsp --disable-proxy --disable-dict \
        --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher \
        --disable-manual --disable-libcurl-option --disable-sspi --disable-ntlm-wb --without-brotli --without-librtmp --without-winidn \
        --disable-threaded-resolver \
        --with-openssl

    make ${makeopts}
    make install
    ldconfig

    popd
}

capnp() {
    git clone https://github.com/opensourcerouting/c-capnproto
    pushd c-capnproto
    git submodule update --init --recursive
    autoreconf -f -i -s

    ./configure
    make ${makeopts}
    make install
    ldconfig

    popd
}

zeroflist() {
    git clone -b development-v2 https://github.com/majjihari/0-flist
    pushd 0-flist

    pushd libflist
    make
    popd

    pushd zflist
    make production
    popd

    cp zflist/zflist /tmp/zflist
    strip -s /tmp/zflist

    popd
}

archive() {
    mkdir -p /tmp/archives
    tar -czvf /tmp/archives/zflist.tar.gz -C /tmp/ zflist
}

pushd /opt

dependencies
capnp
libcurl
zeroflist
archive

popd
