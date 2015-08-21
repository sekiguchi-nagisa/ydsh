#!/usr/bin/env bash


DIR="$(mktemp -d 2> /dev/null || mktemp -d -t ht74583)"

cleanup_tmpdir() {
    rm -rf $DIR
}

ereport() {
    echo trap error in $1
    cleanup_tmpdir
    exit 1
}

trap 'ereport $LINENO' ERR


TARGET=$DIR/hoge.txt

echo hello world > $TARGET


YDSH_BIN=$1

$YDSH_BIN -c "__gets < $TARGET" | grep 'hello world'

$YDSH_BIN -c "grep < $TARGET 'hello world'" | grep 'hello world'

$YDSH_BIN -c "__gets" < $TARGET | grep 'hello world'

cat $TARGET | $YDSH_BIN -c "__gets" | grep 'hello world'

cleanup_tmpdir
exit 0