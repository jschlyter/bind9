#!/bin/sh
#
# Copyright (C) 2014, 2016, 2017  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

SYSTEMTESTTOP=${SYSTEMTESTTOP:=..}
. $SYSTEMTESTTOP/conf.sh

test -r $RANDFILE || $GENRANDOM 800 $RANDFILE

prog=$0

args="-r $RANDFILE"
alg="-a RSAMD5 -b 1024"
quiet=0

msg1="cryptography"
msg2="--with-openssl, or --enable-native-pkcs11 --with-pkcs11"
while test "$#" -gt 0; do
        case $1 in
        -q)
                args="$args -q"
                quiet=1
                ;;
        rsa|RSA)
                alg=""
                msg1="RSA cryptography"
                ;;
        gost|GOST)
                alg="-a eccgost"
                msg1="GOST cryptography"
                msg2="--with-gost"
                ;;
        ecdsa|ECDSA)
                alg="-a ecdsap256sha256"
                msg1="ECDSA cryptography"
                msg2="--with-ecdsa"
                ;;
        *)
                echo "${prog}: unknown argument"
                exit 1
                ;;
        esac
        shift
done


if $KEYGEN $args $alg foo > /dev/null 2>&1
then
    rm -f Kfoo*
else
    if test $quiet -eq 0; then
        echo "I:This test requires support for $msg1" >&2
        echo "I:configure with $msg2" >&2
    fi
    exit 255
fi
