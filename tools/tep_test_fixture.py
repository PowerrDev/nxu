"""Isolated mailbox fixture for GUI integration tests, never a production verifier.
Replies exercise NXU's actual mailbox framing and auth result handling. It does
not validate boot signatures or model tepOS persistence; real enclave tests live
in test_tep_mailbox.sh. No request payloads or passwords are logged.
"""
import hashlib
import os
import select
import socket
import struct
import threading
import time
import zlib


class EnclaveFixture:

    def __init__(self, path, password, transport="socket", password_set=True):
        self.path = str(path)
        self.transport = transport
        self.password_set = password_set
        self.password = password.encode()
        self.stop = threading.Event()
        self.random_calls = 0
        self.denials = 0
        self.accepts = 0
        self.error = None
        self.commands = {}
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)

    def serve(self):
        connection = PipeTransport(self.path) if self.transport == 'pipe' else socket.socket(
            socket.AF_UNIX, socket.SOCK_STREAM
        )
        connection.settimeout(0.5)
        try:
            while not self.stop.is_set():
                try:
                    connection.connect(self.path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    time.sleep(0.05)
            data = bytearray()
            while not self.stop.is_set():
                try:
                    chunk = connection.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                data.extend(chunk)
                while len(data) >= 0x10:
                    if data[:2] != b'TP':
                        del data[0]
                        continue
                    magic, version, kind, command, status, request, length, reserved = struct.unpack_from(
                        '<HBBHHIHH', data
                    )
                    if length > 240:
                        del data[0]
                        continue
                    if len(data) < 20 + length:
                        break
                    packet = bytes(data[:0x10 + length])
                    checksum = struct.unpack_from('<I', data, 0x10 + length)[0]
                    del data[:20 + length]
                    if zlib.crc32(packet) != checksum:
                        continue
                    payload = packet[0x10:]
                    code = 0
                    reply = b''
                    self.commands[command] = self.commands.get(command, 0) + 1
                    if command == 1:
                        reply = struct.pack('<HHII', 1, 7, 0x10000, 1)
                    elif command == 2:
                        reply = bytes([1, 3, 0, 0, 1, 2, 0, 0, 2, 2, 0, 0, 3, 2, 0, 0])
                    elif command == 0x10:
                        reply = hashlib.sha256(payload).digest()
                    elif command == 0x11:
                        size = struct.unpack('<H', payload)[0]
                        reply = os.urandom(size)
                        self.random_calls += 1
                    elif command == 0x32:
                        reply = bytes([int(self.password_set), 0, 0, 0]) + struct.pack('<I', 0)
                    elif command == 0x30:
                        if not self.password_set and payload == b'\0' + self.password:
                            self.password_set = True
                            self.accepts += 1
                        else:
                            code = 8
                            reply = b'\x01'
                            self.denials += 1
                    elif command == 0x31:
                        if payload == self.password:
                            self.accepts += 1
                        else:
                            code = 8
                            reply = b'\x01'
                            self.denials += 1
                    elif command == 0x40:
                        if (
                            len(payload) == 168
                            and payload[:8] == b'TEPBOOT1'
                            and (payload[40:72] == payload[136:168])
                        ):
                            reply = payload[12:0x10]
                        else:
                            code = 8
                            reply = b'\x03'
                    else:
                        code = 2
                    header = struct.pack('<HBBHHIHH', 0x5054, 1, 2, command, code, request, len(reply), 0)
                    response = header + reply
                    connection.sendall(response + struct.pack('<I', zlib.crc32(response)))
        except Exception as error:
            if not self.stop.is_set():
                self.error = str(error)
        finally:
            connection.close()


class PipeTransport:
    """QEMU serial FIFO transport for environments without local sockets."""

    def __init__(self, path):
        self.input = os.open(path + '.out', os.O_RDWR | os.O_NONBLOCK)
        self.output = os.open(path + '.in', os.O_RDWR | os.O_NONBLOCK)
        self.timeout = 0.5

    def settimeout(self, timeout):
        self.timeout = timeout

    def connect(self, path):
        pass

    def recv(self, size):
        if not select.select([self.input], [], [], self.timeout)[0]:
            raise socket.timeout()
        return os.read(self.input, size)

    def sendall(self, data):
        while data:
            if not select.select([], [self.output], [], self.timeout)[1]:
                raise socket.timeout()
            written = os.write(self.output, data)
            data = data[written:]

    def close(self):
        os.close(self.input)
        os.close(self.output)
