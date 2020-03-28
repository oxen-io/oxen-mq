#!/bin/sh

set -e

if ! [ -f debian/changelog ] || ! [ -f debian/control.in ]; then
    if [ -f changelog ] && [ -f control.in ]; then
        cd ..
    else
        echo "Error: must run from base or base/debian directory" >&2
        exit 1
    fi
fi

VERSION=$(head -1 debian/changelog | sed -e 's/.*(//; s/[^0-9.].*//')

sed -e "s/@LIB_VERSION@/$VERSION/g" debian/control.in >debian/control
