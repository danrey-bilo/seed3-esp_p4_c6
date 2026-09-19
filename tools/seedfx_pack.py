#!/usr/bin/env python3
"""Build deterministic SeedFX effect packages and binary pedalboard graphs."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
import math
from generate_seedfx_catalog import STEREO_INPUT, STEREO_OUTPUT
from pathlib import Path

GRAPH_MAGIC = 0x31474653
GRAPH_VERSION = 2
GRAPH_BYTES = 956
MAX_NODES = 12
MAX_EDGES = 48

EFFECT_TYPES = {
    "bypass": 1,
    "gain": 2,
    "mute": 3,
    "polarity": 4,
    "soft_clip": 5,
    "hard_clip": 6,
    "overdrive": 7,
    "fuzz": 8,
    "wavefolder": 9,
    "bitcrusher": 10,
    "sample_rate_reducer": 11,
    "lowpass": 12,
    "highpass": 13,
    "bandpass": 14,
    "notch": 15,
    "bass_boost": 16,
    "treble_boost": 17,
    "compressor": 18,
    "limiter": 19,
    "noise_gate": 20,
    "tremolo": 21,
    "auto_pan": 22,
    "ring_mod": 23,
    "delay": 24,
    "ping_pong_delay": 25,
    "slapback": 26,
    "chorus": 27,
    "flanger": 28,
    "vibrato": 29,
    "phaser": 30,
    "auto_wah": 31,
    "stereo_widener": 32,
    "mono": 33,
    "channel_swap": 34,
    "dc_blocker": 35,
    "reverb": 36,
    "cabinet_sim": 37,
    "exciter": 38,
    "doubler": 39,
    "rotary": 40,
    "splitter": 41,
    "mixer": 42,
}
SHAPES = {"rectangle": 0, "rounded": 1, "pill": 2}


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def build_effect(source: Path, output: Path) -> None:
    manifest = json.loads(source.read_text(encoding="utf-8"))
    body = canonical_json(manifest)
    package_id = int(manifest["package_id"], 0) if isinstance(
        manifest["package_id"], str) else int(manifest["package_id"])
    header_without_crc = struct.pack(
        "<4sHHIIII", b"SFX1", 1, 28, package_id, len(body), 0, 0)
    checksum = zlib.crc32(header_without_crc + struct.pack("<I", 0) + body)
    output.write_bytes(header_without_crc + struct.pack("<I", checksum) + body)


def build_graph(source: Path, output: Path) -> None:
    document = json.loads(source.read_text(encoding="utf-8"))
    nodes = document.get("nodes", [])
    if len(nodes) > MAX_NODES:
        raise ValueError(f"{source}: maximum {MAX_NODES} nodes")
    by_id = {}
    for index, node in enumerate(nodes, 1):
        nid = int(node.get("id", index))
        if not 1 <= nid <= 65535 or nid in by_id or node["type"] not in EFFECT_TYPES:
            raise ValueError("invalid/duplicate node identity")
        by_id[nid] = node

    def ports(nid, output_port):
        if nid == 0: return 2
        if nid not in by_id: raise ValueError("unknown node in connection")
        return 2 if by_id[nid]["type"] in (STEREO_OUTPUT if output_port else STEREO_INPUT) else 1

    routes = document.get("edges")
    if routes is None:
        routes = []
        order = [0] + list(by_id) + [0]
        for a, b in zip(order, order[1:]):
            for dst in range(ports(b, False)):
                selected = int(by_id[b].get("source_channel", 1))-1 if b else 0
                src = 0 if ports(a, True)==1 else (selected if ports(b, False)==1 else dst)
                routes.append({"from":[a,src+1], "to":[b,dst+1]})
    if len(routes)>MAX_EDGES: raise ValueError("too many connections")
    indegree = dict.fromkeys(by_id, 0)
    targets = set()
    adjacency = {nid:[] for nid in by_id}
    edges = bytearray()
    for route in routes:
        a, ap = map(int, route["from"])
        b, bp = map(int, route["to"])
        gain = float(route.get("gain", 1))
        if not 1<=ap<=ports(a,True) or not 1<=bp<=ports(b,False):
            raise ValueError("invalid port")
        if (b,bp) in targets or (a and a==b) or not math.isfinite(gain) or not -4<=gain<=4:
            raise ValueError("duplicate input, self-loop or invalid gain")
        targets.add((b,bp))
        if a and b:
            indegree[b]+=1
            adjacency[a].append(b)
        edges += struct.pack("<HHfBBH",a,b,gain,ap-1,bp-1,0)
    queue=[n for n in by_id if indegree[n]==0]
    for n in queue:
        for dst in adjacency[n]:
            indegree[dst]-=1
            if indegree[dst]==0: queue.append(dst)
    if len(queue)!=len(nodes): raise ValueError("feedback cycles are not supported")
    edges += bytes((MAX_EDGES-len(routes))*12)

    name = document.get("name", source.stem).encode("utf-8")[:23]
    header = struct.pack("<IHHIBBBB24s",GRAPH_MAGIC,GRAPH_VERSION,GRAPH_BYTES,
        int(document.get("revision",1)),int(document.get("enabled",True)),
        len(nodes),len(routes),0,name.ljust(24,bytes(1)))
    node_bytes = bytearray()
    for nid,node in by_id.items():
        effect_type=EFFECT_TYPES[node["type"]]
        values=[float(v) for v in node.get("parameters",[])]
        if len(values)>4 or not all(math.isfinite(v) for v in values):
            raise ValueError("invalid parameters")
        raw_id=node.get("package_id",0x10000+effect_type)
        package_id=int(raw_id,0) if isinstance(raw_id,str) else int(raw_id)
        node_bytes += struct.pack("<HHIBBBB4f",nid,effect_type,package_id,
            int(node.get("enabled",True)),len(values),SHAPES[node.get("shape","rectangle")],
            0,*(values+[0.]*(4-len(values))))
    node_bytes += bytes((MAX_NODES-len(nodes))*28)
    body=header+node_bytes+edges
    assert len(body)==GRAPH_BYTES-4
    output.write_bytes(body+struct.pack("<I",zlib.crc32(body)))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path,
                        default=Path("microSD-ready/SEEDFX"))
    args = parser.parse_args()
    root = args.root
    effect_sources = sorted(root.glob("effects/**/*.json"))
    for source in effect_sources:
        build_effect(source, source.with_suffix(".sfx"))
    for source in sorted((root / "presets").glob("*.json")):
        build_graph(source, source.with_suffix(".sfg"))
    index = {
        "format": "seedfx-card",
        "version": 1,
        "effects": [p.with_suffix(".sfx").relative_to(root).as_posix()
                    for p in effect_sources],
        "presets": [p.with_suffix(".sfg").relative_to(root).as_posix()
                    for p in sorted((root / "presets").glob("*.json"))],
        "autorun": "AUTORUN.SFG",
    }
    (root / "index.json").write_bytes(canonical_json(index))
    autorun = root / "presets" / "empty.sfg"
    (root / "AUTORUN.SFG").write_bytes(autorun.read_bytes())
    packaged = [root / "AUTORUN.SFG", root / "index.json"]
    packaged.extend(sorted(root.glob("effects/**/*.sfx")))
    packaged.extend(sorted(root.glob("presets/*.sfg")))
    checksum_lines = []
    for path in packaged:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        checksum_lines.append(f"{digest}  {path.relative_to(root).as_posix()}")
    (root / "SHA256SUMS.txt").write_text(
        "\n".join(checksum_lines) + "\n", encoding="ascii")
    print(f"SeedFX card image updated: {root.resolve()}")


if __name__ == "__main__":
    main()
