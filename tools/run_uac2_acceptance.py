"""Repeatable Windows hardware test. Never flashes, changes drivers or default endpoints."""
import argparse
import json
import pathlib
import subprocess
import time
from analyze_uac2 import analyze


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", help="Windows capture endpoint GUID")
    parser.add_argument("--render", help="matching P4 render endpoint GUID")
    parser.add_argument("--rate", type=int, choices=(44100, 48000, 88200, 96000), default=48000)
    parser.add_argument("--bits", type=int, choices=(16, 24), default=24)
    parser.add_argument("--period", default="min", help="WASAPI period: min or milliseconds")
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--cycle-seconds", type=float, default=.25)
    parser.add_argument("--record-seconds", type=int, default=60)
    # User shortened the soak to five minutes. One extra second ensures at
    # least 300s of wall time even when the external Seed clock runs slightly fast.
    parser.add_argument("--duplex-seconds", type=int, default=301)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path(__file__).parent / "test-output" / "acceptance")
    args = parser.parse_args()
    exe = pathlib.Path(__file__).parent / "WasapiCapture/bin/Release/net10.0-windows/WasapiCapture.exe"
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "capture": args.capture,
               "render": args.render, "rate": args.rate, "bits": args.bits,
               "period": args.period, "cycles_passed": 0, "completed": False}
    summary["analysis_version"] = 2  # phase discontinuities now fail acceptance

    def save():
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    def record(name, seconds, render=None, measure=False):
        wav = (args.output / f"{name}.wav").resolve()
        command = [str(exe), args.capture, str(wav), str(seconds), "exclusive"]
        if render:
            command.append(render)
        command.extend(["--rate", str(args.rate), "--bits", str(args.bits), "--period", args.period])
        before = time.monotonic()
        # A hung endpoint gets a bounded failure, never an indefinite test wait.
        proc = subprocess.run(command, capture_output=True, timeout=seconds + 25, text=True, errors="replace")
        (args.output / f"{name}.log").write_text(proc.stdout + proc.stderr, encoding="utf-8")
        if proc.returncode:
            raise RuntimeError(f"{name}: recorder exit {proc.returncode}: {proc.stdout[-1200:]} {proc.stderr}")
        result = {"elapsed": time.monotonic() - before, "exit": proc.returncode}
        if measure:
            result["audio"] = analyze(wav, round(seconds * args.rate))
            if not result["audio"]["pass"]:
                (args.output / f"{name}.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
                raise RuntimeError(f"{name}: audio measurements failed; inspect JSON")
        return result

    save()
    try:
        if args.record_seconds:
            print(f"Recording {args.record_seconds}s capture baseline", flush=True)
            summary["capture_record"] = record("capture", args.record_seconds, measure=True)
            save()
        for n in range(args.cycles):
            record(f"cycle-{n + 1:03d}", args.cycle_seconds)
            summary["cycles_passed"] = n + 1
            save()
            if (n + 1) % 10 == 0:
                print(f"Capture open/close: {n + 1}/{args.cycles}", flush=True)
        if args.duplex_seconds:
            if not args.render:
                raise ValueError("--render is required for the full-duplex test")
            print(f"Full-duplex test: {args.duplex_seconds}s", flush=True)
            summary["duplex_record"] = record("duplex", args.duplex_seconds, args.render, True)
            save()
        summary["completed"] = True
        print("HOST TESTS PASS. Independently check P4/Seed serial counters before accepting hardware transport.")
    except Exception as error:
        summary["failure"] = str(error)
        raise
    finally:
        summary["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        save()


if __name__ == "__main__":
    main()
