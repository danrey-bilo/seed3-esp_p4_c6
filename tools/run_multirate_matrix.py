"""Windows format-switch/duplex smoke test. Does not flash or alter default devices."""
import argparse
import json
import pathlib
import re
import subprocess
import time
from analyze_uac2 import analyze
from check_pcm24_capture import check as check_pcm24


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="capture endpoint GUID")
    parser.add_argument("render", help="render endpoint GUID")
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--period", default="min")
    parser.add_argument("--packed24", action="store_true", help="PCM24-only, 3 bytes per USB sample")
    parser.add_argument("--adc", action="store_true", help="layout/count checks only; no tone/analog-quality acceptance (requires --packed24)")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path(__file__).parent / "test-output/multirate-matrix")
    args = parser.parse_args()
    if args.adc and not args.packed24:
        parser.error("--adc requires --packed24")
    if args.seconds < 2:
        parser.error("at least two seconds are needed for waveform analysis")
    exe = pathlib.Path(__file__).parent / "WasapiCapture/bin/Release/net10.0-windows/WasapiCapture.exe"
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "completed": False,
               "capture": args.capture, "render": args.render, "period": args.period, "tests": []}
    summary["analysis_version"] = 2
    summary["packed24"], summary["analog_quality_measured"] = args.packed24, False
    summary["known_tones_required"] = not args.adc
    def save():
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    save()
    try:
        # Deliberately switch clock families and word sizes, then return to 48k.
        for rate in (48000, 44100, 96000, 88200, 48000):
            for bits in ((24,) if args.packed24 else (16, 24)):
                for duplex in (False, True):
                    name = f"{len(summary['tests']) + 1:02d}-{rate}-{bits}-{'duplex' if duplex else 'capture'}"
                    wav = (args.output / f"{name}.wav").resolve()
                    command = [str(exe), args.capture, str(wav), str(args.seconds), "exclusive"]
                    if duplex:
                        command.append(args.render)
                    command += ["--rate", str(rate), "--bits", str(bits), "--period", args.period]
                    if args.packed24:
                        command.append("--packed24")
                    print(name, flush=True)
                    start = time.monotonic()
                    proc = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=args.seconds + 25)
                    log = proc.stdout + proc.stderr
                    (args.output / f"{name}.log").write_text(log, encoding="utf-8")
                    entry = {"name": name, "rate": rate, "bits": bits, "duplex": duplex,
                             "exit": proc.returncode, "elapsed": time.monotonic() - start}
                    summary["tests"].append(entry)
                    if proc.returncode:
                        raise RuntimeError(f"{name}: host process failed ({proc.returncode})")
                    entry["audio"] = (check_pcm24(wav, rate, args.seconds * rate) if args.adc
                                      else analyze(wav, args.seconds * rate))
                    # Keep negotiation, timestamps and final counters in the summary.
                    entry["host"] = [line for line in log.splitlines()
                                     if re.search(r"period |alignment |engine_buffer=|captured=|render_", line)]
                    save()
                    if not entry["audio"]["layout_pass" if args.adc else "pass"]:
                        raise RuntimeError(f"{name}: waveform acceptance failed")
        summary["completed"] = True
        print("FORMAT MATRIX PASS; independently qualify SPI/USB counters and the longer soak.")
    except Exception as error:
        summary["failure"] = str(error)
        raise
    finally:
        summary["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        save()


if __name__ == "__main__":
    main()
