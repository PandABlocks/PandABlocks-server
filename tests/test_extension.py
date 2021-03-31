#!/usr/bin/env python

from __future__ import print_function

import argparse
import sys
import socket


parser = argparse.ArgumentParser(description = 'Run Conversation Test Script')
parser.add_argument(
    '-s', '--server', default = 'localhost',
    help = 'Extension server name, default %(default)s')
parser.add_argument(
    '-p', '--port', default = 9999, type = int,
    help = 'Extension server port, default %(default)d')
parser.add_argument(
    '-q', '--quiet', default = False, action = 'store_true',
    help = 'Only show failed tests')
parser.add_argument(
    'script', help = 'Test script to run')
args = parser.parse_args()


class Script:
    def __init__(self, script):
        self.file = open(script, 'r')
        self.line_no = 0

    def readline(self):
        line = self.file.readline()
        if not line:
            raise StopIteration
        self.line_no += 1
        return line[:-1]

    def readlines(self):
        while True:
            while True:
                line = self.readline()
                if line and line[0] != '#':
                    break
            line_no = self.line_no
            yield (line, self.readline(), line_no)

def script_readlines(script):
    return Script(script).readlines()


class Server:
    def __init__(self, server, port):
        self.sock = socket.socket()
        self.sock.connect((server, port))
        self.sock.settimeout(0.5)

    def exchange(self, line):
        self.sock.sendall(line + '\n')
        result = self.sock.recv(4096)
        assert result[-1] == '\n'
        return result[:-1]


transcript = script_readlines(args.script)
server = Server(args.server, args.port)

error_count = 0
for (command, expected, line_no) in transcript:
    received = server.exchange(command)
    if received != expected:
        if expected:
            print('line %d: expected %r received %r' % (
                line_no, expected, received))
            error_count += 1
        else:
            print('line %d: send %r received %r' % (line_no, command, received))

if error_count > 0:
    print('%d errors' % error_count)
    sys.exit(1)
