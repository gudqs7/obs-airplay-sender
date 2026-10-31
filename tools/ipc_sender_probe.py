#!/usr/bin/env python3
"""Exercise the OBS AirPlay bridge with synthetic H.264 and silence."""

from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import threading
import time
from pathlib import Path

MAGIC = 0x4F415043
HEADER = struct.Struct("<IHHIIQ")
START = 1
VIDEO = 2
AUDIO = 3
STOP = 4
CLOCK = 5
RESPONSE = 100


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("bridge closed the connection")
        data.extend(chunk)
    return bytes(data)


def recv_packet(sock: socket.socket) -> tuple[int, int, bytes]:
    magic, packet_type, _, length, _, pts = HEADER.unpack(recv_exact(sock, HEADER.size))
    if magic != MAGIC:
        raise ValueError(f"invalid IPC magic {magic:#x}")
    return packet_type, pts, recv_exact(sock, length)


def send_packet(sock: socket.socket, lock: threading.Lock, packet_type: int, pts: int = 0,
                payload: bytes = b"") -> None:
    header = HEADER.pack(MAGIC, packet_type, 0, len(payload), 0, pts)
    with lock:
        sock.sendall(header)
        if payload:
            sock.sendall(payload)


def split_access_units(path: Path) -> list[bytes]:
    data = path.read_bytes()
    starts: list[int] = []
    index = 0
    while index + 3 < len(data):
        if data[index:index + 4] == b"\x00\x00\x00\x01":
            starts.append(index)
            index += 4
        elif data[index:index + 3] == b"\x00\x00\x01":
            starts.append(index)
            index += 3
        else:
            index += 1
    if not starts:
        raise ValueError("input has no Annex-B NAL units")

    units: list[bytes] = []
    current = bytearray()
    for start_index, next_index in zip(starts, starts[1:] + [len(data)]):
        nal = data[start_index:next_index]
        offset = 4 if nal.startswith(b"\x00\x00\x00\x01") else 3
        nal_type = nal[offset] & 0x1F
        if nal_type == 9 and current:
            units.append(bytes(current))
            current.clear()
        current.extend(nal)
    if current:
        units.append(bytes(current))
    return units


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--h264", required=True, type=Path)
    parser.add_argument("--target", default="抖音直播伴侣[721246]")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--bitrate", type=int, default=8000)
    parser.add_argument("--seconds", type=float, default=25.0)
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--drop-audio-packets", action="store_true")
    parser.add_argument("--no-encrypt", action="store_true")
    parser.add_argument("--fairplay-key-mode", choices=("", "raw", "mixed"), default="")
    parser.add_argument("--audio-tone-hz", type=float, default=0.0)
    parser.add_argument("--audio-amplitude", type=float, default=0.25)
    args = parser.parse_args()

    units = split_access_units(args.h264)
    print(f"loaded {len(units)} access units from {args.h264}")

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=10)
    sock.settimeout(30)
    send_lock = threading.Lock()

    request = json.dumps({
        "target": args.target,
        "fps": args.fps,
        "bitrate": args.bitrate,
        "noAudio": args.no_audio,
        "dropAudioPackets": args.drop_audio_packets,
        "noEncrypt": args.no_encrypt,
        "fairplayKeyMode": args.fairplay_key_mode,
    }).encode()
    send_packet(sock, send_lock, START, payload=request)
    packet_type, _, payload = recv_packet(sock)
    if packet_type != RESPONSE:
        raise ValueError(f"unexpected response type {packet_type}")
    response = json.loads(payload)
    print("start:", response)
    if not response.get("ok"):
        return 1

    base_ns = time.monotonic_ns()
    send_packet(sock, send_lock, CLOCK, pts=base_ns)

    audio_stop = threading.Event()

    def send_audio() -> None:
        frame_ns = 352 * 1_000_000_000 // 44100
        index = 0
        while not audio_stop.is_set():
            deadline = base_ns + index * frame_ns
            delay = deadline - time.monotonic_ns()
            if delay > 0:
                time.sleep(delay / 1_000_000_000)
            if args.audio_tone_hz > 0:
                amplitude = max(0.0, min(1.0, args.audio_amplitude))
                samples = []
                for sample_index in range(352):
                    phase = 2.0 * math.pi * args.audio_tone_hz * (
                        index * 352 + sample_index
                    ) / 44100.0
                    sample = int(round(math.sin(phase) * amplitude * 32767))
                    samples.extend((sample, sample))
                payload = struct.pack("<704h", *samples)
            else:
                payload = b"\x00" * (352 * 2 * 2)
            send_packet(sock, send_lock, AUDIO, pts=deadline, payload=payload)
            index += 1

    audio_thread = None
    if not args.no_audio and not args.drop_audio_packets:
        audio_thread = threading.Thread(target=send_audio, daemon=True)
        audio_thread.start()

    frame_ns = 1_000_000_000 // args.fps
    total_frames = max(1, int(args.seconds * args.fps))
    started = time.monotonic()
    for index in range(total_frames):
        unit = units[index % len(units)]
        deadline = base_ns + index * frame_ns
        delay = deadline - time.monotonic_ns()
        if delay > 0:
            time.sleep(delay / 1_000_000_000)
        send_packet(sock, send_lock, VIDEO, pts=deadline, payload=unit)

    elapsed = time.monotonic() - started
    print(f"sent {total_frames} video frames in {elapsed:.2f}s")
    time.sleep(2)
    audio_stop.set()
    if audio_thread:
        audio_thread.join(timeout=2)

    sock.settimeout(10)
    send_packet(sock, send_lock, STOP)
    try:
        packet_type, _, payload = recv_packet(sock)
        print("stop:", packet_type, payload.decode(errors="replace"))
    except (ConnectionError, socket.timeout):
        print("bridge stopped without a final response")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
