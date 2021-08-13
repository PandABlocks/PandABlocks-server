#!/usr/bin/env python

from __future__ import print_function

import argparse
import sys
import socket
import time

parser = argparse.ArgumentParser(description = 'Run Conversation Test Script')
parser.add_argument(
    '-s', '--server', default = 'localhost',
    help = 'PandA server name, default %(default)s')
parser.add_argument(
    '-p', '--port', default = 8888, type = int,
    help = 'PandA server port, default %(default)d')
parser.add_argument(
    '-q', '--quiet', default = False, action = 'store_true',
    help = 'Only show failed tests')
parser.add_argument(
    'script', help = 'Test script to run')
args = parser.parse_args()


server = socket.socket()
server.connect((args.server, args.port))
server.settimeout(0.5)

server = server.makefile('rw')

transcript = open(args.script, 'r')


def read_response(count):
    print('Reading response...')
    result = []
    for n in range(count):
        line = server.readline()
        if line:
            result.append(line[:-1])
        else:
            break
    print('finished')
    return result


# Returns next command response set read from transcript file
def transcript_readlines(line_no):
    print('reading transcript lines...')
    to_send = []
    to_receive = []

    # First scan for lines starting with <.
    print('Scanning for lines starting with <')
    for line in transcript:
        line_no += 1
        if line[0] == '<':
            to_send.append(line[2:-1])
        elif line[0] == '>':
            to_receive.append(line[2:-1])
            break

    # Now read the remainder of the response
    print('Reading the remainder of the response...')
    for line in transcript:
        line_no += 1
        if line[0] == '>':
            to_receive.append(line[2:-1])
        elif line[0] == '#':
            # Allow inline comments in respone
            pass
        else:
            break
    print('finished')        
    return (to_send, to_receive, line_no)


failed = 0
line_no = 0
print('Entering while true...')
while True:
    (tx, rx, line_no) = transcript_readlines(line_no)
    if not tx:
        print('Leaving while true...')
        break

    start = time.time()
    for line in tx:
        server.write(line + '\n')
    server.flush()
    response = read_response(len(rx))
    end = time.time()

    if response == rx:
        if not args.quiet:
            print(tx[0], 'OK %.2f ms' % (1e3 * (end - start)))
    else:
        print(tx[0], 'response error', response, 'on line', line_no)
        failed += 1

if failed:
    print(failed, 'tests failed')
    sys.exit(1)
else:
    print('all ok')
