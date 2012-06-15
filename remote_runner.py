#!/usr/bin/python

import sys
import subprocess
import os
import shutil
import time
import pymongo
import json
import pprint
import datetime
from pymongo.json_util import object_hook
from optparse import OptionParser

optparser = OptionParser()
optparser.add_option('-p', '--hostport', dest='hostport', help='host+port for mongodb to test', type='string', default='127.0.0.1:30027')
optparser.add_option('-d', '--results-hostport', dest='results_hostport', help='host+port for mongodb to store results in', type='string', default='127.0.0.1:27017')
optparser.add_option('-n', '--iterations', dest='iterations', help='number of iterations to test', type='string', default='100000')
optparser.add_option('-s', '--mongos', dest='mongos', help='send all requests through mongos', action='store_true', default=False)
optparser.add_option('-m', '--multidb', dest='multidb', help='use a separate db for each connection', action='store_true', default=False)
optparser.add_option('-l', '--label', dest='label', help='name to record', type='string', default='<git version>')
optparser.add_option('-r', '--remote', dest='remote', help='remote machine to scp and run on', action='append')
optparser.add_option('-b', '--build', dest='build', help='do build', action='store_true', default=False)

(opts, versions) = optparser.parse_args()
if not versions:
    versions = ['master']
if not opts.remote:
    optparser.error('you must pass -r to indicate the remote hosts you want to run this on.')

#TODO support multiple versions
branch = versions[0]
mongodb_version = (branch if opts.label=='<git version>' else opts.label)
mongodb_date = None

mongod = None # set in following block
mongodb_git="nolaunch"

if opts.label != '<git version>':
    mongodb_git = opts.label

if opts.build:
  remote_builds = []
  for r in opts.remote:
    scp = subprocess.Popen(['scp', './clone_and_build.sh', '%s:' % r])
    if scp.wait() != 0:
      raise Exception('Couldn\'t scp benchmark to %s' % r)
    print 'scp\'d bechmark to %s' % r
    remote_builds.append([r, subprocess.Popen(['ssh', r, './clone_and_build.sh'])])
  for (rh, rb) in remote_builds:
    if rb.wait() != 0:
      raise Exception('Couldn\'t do remote build on %s :(' % rh)
      [ x.terminate() for x in remote_builds ]

remote_runs = []
for r in opts.remote:
  remote_runs.append(subprocess.Popen(['ssh', r, './perfrunner/mongo-perf/benchmark', opts.hostport, opts.iterations, '1' if opts.multidb else '0'], stdout=subprocess.PIPE))

benchmark_results=''
for rr in remote_runs:
  ret = rr.communicate()[0]
  benchmark_results = benchmark_results + '\n' + ret

connection = None
try:
    connection = pymongo.Connection(host = opts.results_hostport)
    results = connection.bench_results.raw
    results.ensure_index('mongodb_git')
    results.ensure_index('name')
    results.remove({'mongodb_git': mongodb_git})
except pymongo.errors.ConnectionFailure:
    pass


for line in benchmark_results.split('\n'):
    if line and not ('DBClientConnection::call' in line) and not ('mongo::MsgAssertionException' in line):
        print line
        obj = json.loads(line, object_hook=object_hook)
        obj['mongodb_version'] = mongodb_version
        obj['mongodb_date'] = mongodb_date
        obj['mongodb_git'] = mongodb_git
        obj['ran_at'] = datetime.datetime.now()
        if connection: results.insert(obj)





