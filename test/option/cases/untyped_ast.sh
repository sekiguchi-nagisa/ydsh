#!/usr/bin/env bash

ereport() {
    echo trap error in $1
    exit 1
}

trap 'ereport $LINENO' ERR

YDSH_BIN=$1

$YDSH_BIN --dump-untyped-ast -c '12' | grep '### dump untyped AST ###'

exit 0