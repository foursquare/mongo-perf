#!/bin/bash

set -e
set -x

mkdir -p perfrunner
cd perfrunner
if [ -d mongo ]; then
  cd mongo
  git checkout HEAD shell/mongo_vstudio.cpp
  git checkout r2.0.5-fs
  git pull
else
  git clone git@github.com:foursquare/mongo.git
  cd mongo
  git checkout r2.0.5-fs
fi
scons --full all
scons --full mongoclient
cd ..


if ! [ -d mongo-perf ]; then
  git clone git@github.com:foursquare/mongo.git
fi
cd mongo-perf
git pull
scons
