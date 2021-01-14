#!/bin/bash

set -e

UPSTREAM_VER="$1"
LIB_VER="${UPSTREAM_VER/[^0-9.]*/}"
if ! grep -q "^Package: liboxenmq$LIB_VER\$" debian/control; then
    echo -e "\nError: debian/control doesn't contain the correct liboxenmq$LIB_VER version; you should run:\n\n    ./debian/update-lib-version.sh\n"
    exit 1
fi

if ! [ -f debian/liboxenmq$LIB_VER ]; then
    rm -f debian/liboxenmq[0-9]*.install
    sed -e "s/@LIB_VER@/$LIB_VER/" debian/liboxenmq.install.in >debian/liboxenmq$LIB_VER.install
fi
