#!/usr/bin/env bash
set -e
set -u


BASEDIR=$(dirname $(realpath "$0"))

cd $BASEDIR

sudo rm -rf ../build
mkdir ../build

docker build -t tmp .
docker run --rm -v "$PWD"/..:/github/workspace tmp
