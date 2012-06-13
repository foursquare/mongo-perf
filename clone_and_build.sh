#!/bin/bash

set -e
set -x

mkdir -p perfrunner
cd perfrunner
git clone git@github.com:foursquare/mongo.git
cd mongo
git checkout r2.0.5-fs
scons --full all
scons --full mongoclient
cd ..
git clone git@github.com:foursquare/mongo-perf.git
cd mongo-perf
scons
