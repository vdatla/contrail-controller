#!/usr/bin/env python

#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

#
# mockcassandra
#
# This module helps start and stop cassandra instances for unit testing
# java must be pre-installed for this to work
#
    
import os
import subprocess
import logging
import socket

logging.basicConfig(level=logging.INFO,
                            format='%(asctime)s %(levelname)s %(message)s')

cassandra_version = '1.2.11'
cassandra_url = '/tmp/apache-cassandra-'+cassandra_version+'-bin.tar.gz'

def start_cassandra(cport, sport_arg=None):
    '''
    Client uses this function to start an instance of Cassandra
    Arguments:
        cport : An unused TCP port for Cassandra to use as the client port
    '''
    cassandra_download = 'curl -o ' +\
        cassandra_url + ' -s -m 120 http://archive.apache.org/dist/cassandra/'+\
        cassandra_version+'/apache-cassandra-'+cassandra_version+'-bin.tar.gz'
    if not os.path.exists(cassandra_url):
        process = subprocess.Popen(cassandra_download.split(' '))
        process.wait()
        if process.returncode is not 0:
            return

    basefile = 'apache-cassandra-'+cassandra_version
    tarfile = cassandra_url
    cassbase = "/tmp/cassandra." + str(cport) + "/"
    confdir = cassbase + basefile + "/conf/"
    output,_ = call_command_("mkdir " + cassbase)

    logging.info('Installing cassandra in ' + cassbase)
    os.system("cat " + tarfile + " | tar -xpzf - -C " + cassbase)

    output,_ = call_command_("mkdir " + cassbase + "commit/")
    output,_ = call_command_("mkdir " + cassbase + "data/")
    output,_ = call_command_("mkdir " + cassbase + "saved_caches/")

    if not sport_arg:
        ss = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ss.bind(("",0))
        sport = ss.getsockname()[1]
    else:
        sport = sport_arg

    js = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    js.bind(("",0))
    jport = js.getsockname()[1]

    logging.info('Cassandra Client Port %d' % cport)

    replace_string_(confdir + "cassandra.yaml", \
        [("rpc_port: 9160","rpc_port: " + str(cport)), \
        ("storage_port: 7000","storage_port: " + str(sport))])

    replace_string_(confdir + "cassandra.yaml", \
        [("/var/lib/cassandra/data",  cassbase + "data"), \
        ("/var/lib/cassandra/commitlog",  cassbase + "commitlog"), \
        ("/var/lib/cassandra/saved_caches",  cassbase + "saved_caches")])

    replace_string_(confdir + "log4j-server.properties", \
       [("/var/log/cassandra/system.log", cassbase + "system.log"),
        ("INFO","DEBUG")])

    replace_string_(confdir + "cassandra-env.sh", \
        [('JMX_PORT="7199"', 'JMX_PORT="' + str(jport) + '"')])

    replace_string_(confdir + "cassandra-env.sh", \
        [('#MAX_HEAP_SIZE="4G"', 'MAX_HEAP_SIZE="256M"'), \
        ('#HEAP_NEWSIZE="800M"', 'HEAP_NEWSIZE="100M"')])

    if not sport_arg:
        ss.close()

    js.close()
    output,_ = call_command_(cassbase + basefile + "/bin/cassandra -p " + cassbase + "pid")

    return cassbase, basefile


def stop_cassandra(cport):
    '''
    Client uses this function to stop an instance of Cassandra
    This will only work for cassandra instances that were started by this module
    Arguments:
        cport : The Client Port for the instance of cassandra to be stopped
    '''
    cassbase = "/tmp/cassandra." + str(cport) + "/"
    input = open(cassbase + "pid")
    s=input.read()
    logging.info('Killing Cassandra pid %d' % int(s))
    output,_ = call_command_("kill -9 %d" % int(s))
    output,_ = call_command_("rm -rf " + cassbase)
    
def replace_string_(filePath, findreplace):
    "replaces all findStr by repStr in file filePath"
    print filePath
    tempName=filePath+'~~~'
    input = open(filePath)
    output = open(tempName,'w')
    s=input.read()
    for couple in findreplace:
        outtext=s.replace(couple[0],couple[1])
        s=outtext
    output.write(outtext)
    output.close()
    input.close()
    os.rename(tempName,filePath)

def call_command_(command):
    process = subprocess.Popen(command.split(' '),
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    return process.communicate()

if __name__ == "__main__":
    cs = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    cs.bind(("",0))
    cport = cs.getsockname()[1]
    cs.close()
    start_cassandra(cport)


