#!/usr/bin/env python3
"""Ready-made Python implementation of the WAIL wire protocol v1 (see ../../protocol.md).

Use it if you prefer to write the client in Python (standard library only, Python 3.11+):

    from wail_protocol import (Header, Hello, Input, Ping, Bye, encode, decode, mono_ns,
                               Welcome, State, Pong)
    sock.sendto(encode(Header(seq=1), Hello(nonce=123, requested_id=2)), ("127.0.0.1", 40000))
    hdr, msg = decode(data)          # returns None for malformed datagrams
    if isinstance(msg, State): ...

Self-test (golden bytes from protocol.md):  python3 -I wail_protocol.py
"""

from __future__ import annotations

import struct
import sys
import time
from dataclasses import dataclass, field

MAGIC = 0x4157
VERSION = 1
MAX_DATAGRAM = 1200
HEADER = struct.Struct("<HBBIQ")  # magic, version, type, seq, send_ns  (16 bytes)

KEY_W, KEY_A, KEY_S, KEY_D, KEY_SPACE, KEY_SHIFT, KEY_MOUSE1, KEY_MOUSE2 = (1 << i for i in range(8))


def mono_ns() -> int:
    """Monotonic clock in ns that matches wail::mono_ns() (C++ std::chrono::steady_clock) of the server.

    Linux: CLOCK_MONOTONIC for both. macOS: libc++ steady_clock is CLOCK_MONOTONIC_RAW, while Python's
    time.monotonic_ns() is a DIFFERENT clock (CLOCK_UPTIME_RAW) with another epoch, so we read
    CLOCK_MONOTONIC_RAW explicitly. Values of different processes on one host are comparable.
    Windows has no clock_gettime_ns: there time.perf_counter_ns() is used (QueryPerformanceCounter, the
    same source as MSVC steady_clock). The server itself builds on POSIX only, so on Windows use WSL2.
    """
    if not hasattr(time, "clock_gettime_ns"):
        return time.perf_counter_ns()
    if sys.platform == "darwin":
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


@dataclass
class Header:
    seq: int = 0
    send_ns: int = 0  # filled by encode() with mono_ns() if left 0


@dataclass
class Hello:  # type 1, client -> server
    nonce: int = 0
    requested_id: int = 0
    TYPE = 1
    FMT = struct.Struct("<IHH")

    def pack(self) -> bytes:
        return self.FMT.pack(self.nonce, self.requested_id, 0)


@dataclass
class Welcome:  # type 2, server -> client
    nonce: int = 0
    client_id: int = 0
    tick_hz: int = 0
    server_tick: int = 0
    TYPE = 2
    FMT = struct.Struct("<IHHI")


@dataclass
class Input:  # type 3, client -> server
    client_id: int = 0
    keys: int = 0
    input_seq: int = 0
    client_frame: int = 0
    mouse_dx: int = 0
    mouse_dy: int = 0
    input_ns: int = 0
    TYPE = 3
    FMT = struct.Struct("<HHIIhhQ")

    def pack(self) -> bytes:
        return self.FMT.pack(self.client_id, self.keys, self.input_seq, self.client_frame,
                             self.mouse_dx, self.mouse_dy, self.input_ns)


@dataclass
class PlayerState:
    client_id: int
    keys: int
    last_input_seq: int
    last_input_recv_ns: int  # SERVER clock
    pos_x: float
    pos_y: float
    vel_x: float
    vel_y: float
    FMT = struct.Struct("<HHIQffff")


@dataclass
class State:  # type 4, server -> clients
    server_tick: int = 0
    tick_start_ns: int = 0  # SERVER clock
    players: list[PlayerState] = field(default_factory=list)
    TYPE = 4
    FMT = struct.Struct("<IB3xQ")


@dataclass
class Ping:  # type 5, client -> server
    ping_id: int = 0
    t0_ns: int = 0
    TYPE = 5
    FMT = struct.Struct("<IIQ")

    def pack(self) -> bytes:
        return self.FMT.pack(self.ping_id, 0, self.t0_ns)


@dataclass
class Pong:  # type 6, server -> client
    ping_id: int = 0
    t0_ns: int = 0
    t1_ns: int = 0  # server: Ping reached the server logic
    t2_ns: int = 0  # server: Pong left the server logic
    TYPE = 6
    FMT = struct.Struct("<IIQQQ")


@dataclass
class Bye:  # type 7, either direction
    client_id: int = 0
    reason: int = 0
    TYPE = 7
    FMT = struct.Struct("<HH")

    def pack(self) -> bytes:
        return self.FMT.pack(self.client_id, self.reason)


def encode(header: Header, msg) -> bytes:
    """Serializes a client -> server message (Hello, Input, Ping, Bye)."""
    send_ns = header.send_ns or mono_ns()
    return HEADER.pack(MAGIC, VERSION, msg.TYPE, header.seq & 0xFFFFFFFF, send_ns) + msg.pack()


def decode(data: bytes):
    """Parses one datagram. Returns (Header, message) or None if the datagram is malformed."""
    if len(data) < HEADER.size or len(data) > MAX_DATAGRAM:
        return None
    magic, version, mtype, seq, send_ns = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        return None
    body = memoryview(data)[HEADER.size:]
    hdr = Header(seq=seq, send_ns=send_ns)
    try:
        if mtype == State.TYPE:
            if len(body) < State.FMT.size:
                return None
            tick, count, start = State.FMT.unpack_from(body, 0)
            if count > 16 or len(body) != State.FMT.size + count * PlayerState.FMT.size:
                return None
            players = [PlayerState(*PlayerState.FMT.unpack_from(body, State.FMT.size + i * PlayerState.FMT.size))
                       for i in range(count)]
            return hdr, State(tick, start, players)
        if mtype == Welcome.TYPE and len(body) == Welcome.FMT.size:
            return hdr, Welcome(*Welcome.FMT.unpack(body))
        if mtype == Pong.TYPE and len(body) == Pong.FMT.size:
            pid, _r, t0, t1, t2 = Pong.FMT.unpack(body)
            return hdr, Pong(pid, t0, t1, t2)
        if mtype == Bye.TYPE and len(body) == Bye.FMT.size:
            return hdr, Bye(*Bye.FMT.unpack(body))
        if mtype == Input.TYPE and len(body) == Input.FMT.size:
            return hdr, Input(*Input.FMT.unpack(body))
        if mtype == Hello.TYPE and len(body) == Hello.FMT.size:
            nonce, rid, _r = Hello.FMT.unpack(body)
            return hdr, Hello(nonce, rid)
        if mtype == Ping.TYPE and len(body) == Ping.FMT.size:
            pid, _r, t0 = Ping.FMT.unpack(body)
            return hdr, Ping(pid, t0)
    except struct.error:
        return None
    return None


def _selftest() -> int:
    golden = bytes.fromhex("5741010307000000080706050403020102004100e8030000370000" "00fdff0400" "8877665544332211")
    got = encode(Header(seq=7, send_ns=0x0102030405060708),
                 Input(client_id=2, keys=KEY_W | KEY_MOUSE1, input_seq=1000, client_frame=55,
                       mouse_dx=-3, mouse_dy=4, input_ns=0x1122334455667788))
    assert got == golden, got.hex()
    hdr, msg = decode(got)
    assert hdr.seq == 7 and isinstance(msg, Input) and msg.input_seq == 1000 and msg.mouse_dx == -3
    assert decode(got[:-1]) is None and decode(b"\x00" * 16) is None
    st = HEADER.pack(MAGIC, VERSION, 4, 1, 2) + State.FMT.pack(5, 1, 99) + PlayerState.FMT.pack(3, 1, 42, 7, 1, 2, 3, 4)
    hdr, msg = decode(st)
    assert isinstance(msg, State) and msg.players[0].last_input_seq == 42 and msg.tick_start_ns == 99
    print("wail_protocol selftest: OK (mono_ns = %d)" % mono_ns())
    return 0


if __name__ == "__main__":
    sys.exit(_selftest())
