#!/usr/bin/env python


import argparse
import socket
import sys

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
        self.file = open(script)
        self.line_no = 0

    def readline(self):
        line = self.file.readline()
        if not line:
            raise StopIteration()
        self.line_no += 1
        return line[:-1]

    def readlines(self):
        try:
            while True:
                while True:
                    line = self.readline()
                    if line and line[0] != '#':
                        break
                line_no = self.line_no
                yield (line, self.readline(), line_no)
        except StopIteration:
            # This special handling of StopIteration is mandated by PEP 479
            return

def script_readlines(script):
    return Script(script).readlines()


class Server:
    def __init__(self, server, port):
        self.sock = socket.socket()
        self.sock.connect((server, port))
        self.sock.settimeout(0.5)

    def exchange(self, line):
        self.sock.sendall((line + '\n').encode())
        result = self.sock.recv(4096).decode()
        assert result[-1] == '\n'
        return result[:-1]


transcript = script_readlines(args.script)
server = Server(args.server, args.port)

error_count = 0
for (command, expected, line_no) in transcript:
    received = server.exchange(command)
    if received != expected:
        if expected:
            print(f'line {line_no}: expected {expected!r} received {received!r}')
            error_count += 1
        else:
            print(f'line {line_no}: send {command!r} received {received!r}')

if error_count == 0:
    print('Test ok', file = sys.stderr)
    sys.exit(0)
else:
    print(f'{error_count} errors', file = sys.stderr)
    sys.exit(1)
