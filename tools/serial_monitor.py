#!/usr/bin/env python3
"""Minimal binary-safe serial monitor used during USB audio tests."""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=0, help="seconds; 0 runs until interrupted")
    parser.add_argument("--output", help="also save raw serial bytes to this file")
    args = parser.parse_args()

    device = serial.Serial(port=None, baudrate=args.baud, timeout=0.2)
    device.dtr = False
    device.rts = False
    device.port = args.port
    log = open(args.output, "wb") if args.output else None
    try:
        device.open()
        start = time.monotonic()
        while not args.duration or time.monotonic() - start < args.duration:
            payload = device.read(4096)
            if payload:
                if log:
                    log.write(payload)
                    log.flush()
                sys.stdout.write(payload.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        device.close()
        if log:
            log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
