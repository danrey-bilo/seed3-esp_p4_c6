"""P4 diagnostic console: no DTR/RTS reset, bounded observation, optional command."""
import argparse
import pathlib
import sys
import time
import serial


def main():
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--command", help="buffer 1|2|4, transport balanced|low, seed reset|boot|stats, spi pause N, ui stress")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.command:
        fixed = args.command in (
            "buffer 1", "buffer 2", "buffer 4",
            "transport balanced", "transport low",
            "seed reset", "seed boot", "seed stats",
            "ui stress", "wifi scan", "wifi status",
        )
        pause = args.command.startswith("spi pause ") and args.command[10:].isdigit() and 1 <= int(args.command[10:]) <= 10000
        if not fixed and not pause:
            parser.error("unsupported command")
    device = serial.Serial(port=None, baudrate=115200, timeout=.1)
    device.dtr = device.rts = False
    device.port = args.port
    output = None
    try:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            output = args.output.open("wb")
        device.open()
        if args.command:
            # CH343 may discard the first byte immediately after opening.
            time.sleep(.15)
            device.write(b"\n")
            device.write((args.command + "\n").encode("ascii"))
            device.flush()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = device.read(4096)
            if data:
                if output: output.write(data)
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        device.close()
        if output: output.close()


if __name__ == "__main__":
    main()
