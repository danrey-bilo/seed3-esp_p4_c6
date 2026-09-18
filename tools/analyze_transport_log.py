"""Summarize P4 UART counters and CPU estimates without changing any device."""
import argparse
import json
from pathlib import Path
import re
import statistics


def fields(line):
    return {key: int(value, 16 if value.startswith("0x") else 10)
            for key, value in re.findall(r"\b(\w+)=(0x[0-9a-fA-F]+|[0-9]+)", line)}


def analyze(path):
    records, cpu, spi = [], [], []
    current_format = {}
    last_usb = None
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        timestamp = re.search(r"I \((\d+)\)", line)
        if not timestamp:
            continue
        ms = int(timestamp[1])
        if "frame_err[h/c/s/f]=" in line:
            errors = re.search(r"frame_err\[h/c/s/f\]=(\d+)/(\d+)/(\d+)/(\d+)", line)
            spi.append({"ms": ms, **fields(line), "frame_errors": [int(x) for x in errors.groups()]})
        elif "format rate=" in line:
            current_format = fields(line)
            bits = re.search(r"bits=(\d+)/(\d+)", line)
            current_format["capture_bits"], current_format["playback_bits"] = map(int, bits.groups())
        elif "uac mounted=" in line:
            active = re.search(r"active=(\d)/(\d)", line)
            last_usb = {"ms": ms, **current_format, **fields(line),
                        "active": [int(active[1]), int(active[2])]}
            # 'over' occurs twice; do not conflate capture and playback.
            for key, pattern in {
                "capture_over": r"sil=\d+ over=(\d+)",
                "playback_over": r"under=\d+ over=(\d+)",
                "capture_fill": r"fill=(\d+)\+", "capture_queued": r"fill=\d+\+(\d+)",
                "playback_fill": r"play_read=\d+ fill=(\d+)",
                "capture_gap": r"gap=(\d+)/", "playback_gap": r"gap=\d+/(\d+)"
            }.items():
                last_usb[key] = int(re.search(pattern, line)[1])
            for ambiguous in ("fill", "over", "gap", "bits"):
                last_usb.pop(ambiguous, None)
            records.append(last_usb)
        elif "cpu permille" in line and last_usb:
            cpu.append({"ms": ms, "rate": current_format.get("rate"),
                        "active": last_usb["active"], **fields(line)})

    runs, run = [], []
    for row in records:
        signature = (row.get("rate"), row.get("capture_bits"), row.get("playback_bits"), row["active"])
        if run and (signature != run[-1][0] or row["ms"] - run[-1][1]["ms"] > 2000):
            runs.append(run)
            run = []
        run.append((signature, row))
    if run:
        runs.append(run)
    steady = []
    counters = ("sil", "capture_over", "under", "playback_over", "bad", "inc", "recover", "reset", "fb_inc")
    for run in runs:
        # Exclude two startup samples and the last sample before close.
        if len(run) < 6 or run[0][1]["active"] != [1, 1]:
            continue
        first, last = run[2][1], run[-2][1]
        delta = {key: (last[key] - first[key]) & 0xffffffff for key in counters}
        steady.append({"rate": first["rate"], "bits": [first["capture_bits"], first["playback_bits"]],
                       "from_ms": first["ms"], "to_ms": last["ms"], "duration_ms": last["ms"] - first["ms"],
                       "deltas": delta, "capture_gap_max_us": last["capture_gap"] * 125,
                       "playback_gap_max_us": last["playback_gap"] * 125})
    cpu_summary = {}
    for rate in sorted({r["rate"] for r in cpu}):
        subset = [r for r in cpu if r["rate"] == rate and r["active"] == [1, 1]]
        if subset:
            cpu_summary[str(rate)] = {key: {"min_percent": min(r[key] for r in subset) / 10,
                                           "median_percent": statistics.median(r[key] for r in subset) / 10,
                                           "max_percent": max(r[key] for r in subset) / 10}
                                      for key in ("busy_core0", "busy_core1", "spi_task", "usb_task")}
    clean_spi = bool(spi) and all(r["err"] == 0 and not any(r["frame_errors"]) for r in spi)
    # Preserve absolute counters above: a pre-test startup error is not zero.
    # Also expose interval deltas without confusing old errors with new ones.
    spi_delta = None
    if spi:
        spi_delta = {"transfer_errors": (spi[-1]["err"] - spi[0]["err"]) & 0xffffffff,
                     "frame_errors_h_c_s_f": [(last - first) & 0xffffffff
                                              for first, last in zip(spi[0]["frame_errors"], spi[-1]["frame_errors"])]}
    unchanged_spi = (len(spi) >= 2 and not spi_delta["transfer_errors"]
                     and not any(spi_delta["frame_errors_h_c_s_f"]))
    clean_steady = bool(steady) and all(not any(r["deltas"].values()) for r in steady)
    return {"spi_samples": len(spi), "spi_errors_all_zero": clean_spi,
            "spi_error_deltas": spi_delta, "spi_error_counters_unchanged": unchanged_spi,
            "spi_first": spi[0] if spi else None, "spi_last": spi[-1] if spi else None,
            "usb_first": records[0] if records else None, "usb_last": records[-1] if records else None,
            "steady_duplex": steady, "steady_duplex_counters_unchanged": clean_steady,
            "cpu_scheduler_estimate": cpu_summary,
            "note": "CPU estimates can attribute ISR time to interrupted tasks. WAV/host tests are separate requirements."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = analyze(args.log)
    text = json.dumps(result, indent=2)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(text + "\n", encoding="utf-8")
    print(text)
