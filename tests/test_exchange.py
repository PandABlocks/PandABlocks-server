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


def read_one_response():
    line = server.readline()[:-1]
    result = [line]
    if line[0] == '!':
        # Read complete multi-line response
        while True:
            line = server.readline()[:-1]
            result.append(line)
            if line == '.':
                break
    return result


def read_response(count):
    result = []
    for n in range(count):
        result.extend(read_one_response())
    return result


# Returns next command response set read from transcript file
def transcript_readlines(line_no):
    to_send = []
    to_receive = []
    count = 0

    # First scan for lines starting with <.
    # We also need to count the number of full commands sent.  This is
    # complicated when we send a table.
    in_table = False
    for line in transcript:
        line_no += 1
        if line[0] == '<':
            to_send.append(line[2:-1])
            if in_table:
                in_table = bool(line[2:-1])
            else:
                count += 1
                in_table = '<' in line[2:-1]
        elif line[0] == '>':
            to_receive.append(line[2:-1])
            assert not in_table
            break

    # Now read the remainder of the response
    for line in transcript:
        line_no += 1
        if line[0] == '>':
            to_receive.append(line[2:-1])
        elif line[0] == '#':
            # Allow inline comments in respone
            pass
        else:
            break

    return (to_send, to_receive, count, line_no)


failed = 0
line_no = 0
while True:
    (tx, rx, count, line_no) = transcript_readlines(line_no)
    if not tx:
        break

    start = time.time()
    for line in tx:
        server.write(line + '\n')
    server.flush()
    response = read_response(count)
    end = time.time()

    if response == rx:
        if not args.quiet:
            print(tx[0], 'OK %.2f ms' % (1e3 * (end - start)))
    else:
        print(tx[0], 'response error', response, 'on line', line_no)
        failed += 1

if failed:
    print(failed, 'exchange tests failed')
    sys.exit(1)
else:
    print('all ok')
