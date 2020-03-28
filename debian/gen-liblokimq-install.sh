#!/bin/bash

set -e

UPSTREAM_VER="$1"
LIB_VER="${UPSTREAM_VER/[^0-9.]*/}"
if ! grep -q "^Package: liblokimq$LIB_VER\$" debian/control; then
    echo -e "\nError: debian/control doesn't contain the correct liblokimq$LIB_VER version; you should run:\n\n    ./debian/update-lib-version.sh\n"
    exit 1
fi

if ! [ -f debian/liblokimq$LIB_VER ]; then
    rm -f debian/liblokimq[0-9]*.install
    sed -e "s/@LIB_VER@/$LIB_VER/" debian/liblokimq.install.in >debian/liblokimq$LIB_VER.install
fi
