"""Copies a music folder (made by prepare_sd.py) onto the player's SD card over the USB cable.

Usage:  python3 upload_music.py <folder>/music [--port /dev/cu.xxx] [--baud 3000000]

The player must be running the SD_Player firmware. Files already on the card with the same size
are skipped, so if it's interrupted, just run it again to carry on. Each file is checked with a
CRC after copying and sent again if it doesn't match. The player restarts at the end.
Needs pyserial.
"""
import argparse
import glob
import os
import sys
import time
import zlib

import serial

CHUNK = 16384   # must match receiveFile() in the firmware


def answer(s, prefix, timeout=10):
    """Waits for a line starting with '@'; other lines (the player's log) are ignored."""
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="ignore").strip()
        if line.startswith("@"):
            if not line.startswith(prefix):
                raise IOError(line)
            return line
    raise IOError(f"no answer (waiting for {prefix})")


def send(s, cmd):
    s.write((cmd + "\n").encode())


def put(s, local, remote):
    size = os.path.getsize(local)
    send(s, f"PUT {size} {remote}")
    answer(s, "@GO")
    crc = 0
    with open(local, "rb") as f:
        while True:
            data = f.read(CHUNK)
            if not data:
                break
            crc = zlib.crc32(data, crc)
            s.write(data)
            answer(s, "@K", timeout=15)
    got = answer(s, "@CRC").split()[1]
    if int(got, 16) != crc:
        raise IOError(f"CRC mismatch ({got} vs {crc:08x})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("music", help="the music folder made by prepare_sd.py")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=3000000)
    args = ap.parse_args()

    root = os.path.abspath(args.music)
    files = []
    for d, _, names in os.walk(root):
        for n in sorted(names):
            if n.startswith("."):
                continue
            local = os.path.join(d, n)
            files.append((local, "/music/" + os.path.relpath(local, root)))
    # album.txt goes last in each album, so a half-copied album isn't listed on the player
    files.sort(key=lambda x: (os.path.dirname(x[1]), x[1].endswith("album.txt")))
    total = sum(os.path.getsize(f) for f, _ in files)

    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*"))
    port = args.port or (ports[0] if len(ports) == 1 else sys.exit(f"Found ports {ports}; pick one with --port"))

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False   # don't reset the board when opening the port
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    send(s, "")
    send(s, "PING")
    answer(s, "@PONG")
    send(s, f"BAUD {args.baud}")
    answer(s, "@OK")
    time.sleep(0.1)
    s.baudrate = args.baud
    time.sleep(0.1)
    s.reset_input_buffer()
    send(s, "")
    send(s, "PING")
    answer(s, "@PONG")
    print(f"Connected at {args.baud} baud. {len(files)} files, {total / 1e6:.0f} MB", flush=True)

    done_bytes, sent_bytes, t0 = 0, 0, time.time()
    for i, (local, remote) in enumerate(files, 1):
        size = os.path.getsize(local)
        send(s, f"STAT {remote}")
        if int(answer(s, "@SIZE").split()[1]) == size:
            done_bytes += size
            continue
        send(s, f"MSG RECEIVING {i}/{len(files)}")
        for attempt in range(3):
            try:
                put(s, local, remote)
                break
            except IOError as e:
                print(f"  {remote}: {e}, retrying", flush=True)
                time.sleep(3.5)   # let the player give up on the broken transfer
                s.reset_input_buffer()
                send(s, "")
                send(s, "PING")
                answer(s, "@PONG")
        else:
            sys.exit(f"Failed to copy {remote}")
        done_bytes += size
        sent_bytes += size
        rate = sent_bytes / (time.time() - t0)
        eta = (total - done_bytes) / rate if rate else 0
        print(f"{done_bytes / total * 100:5.1f}%  {remote}  ({rate / 1e3:.0f} KB/s, {eta / 60:.0f} min left)", flush=True)

    print("All files copied. Restarting the player.", flush=True)
    send(s, "REBOOT")
    try:
        answer(s, "@OK", timeout=3)
    except IOError:
        pass


if __name__ == "__main__":
    main()
