import importlib.util
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "seedfx_pack", Path(__file__).with_name("seedfx_pack.py"))
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)


class SeedFxPackTests(unittest.TestCase):
    def pack_document(self, document):
        with tempfile.TemporaryDirectory() as folder:
            source=Path(folder)/"test.json"
            source.write_text(json.dumps(document),encoding="utf-8")
            output=source.with_suffix(".sfg")
            PACK.build_graph(source,output)
            return output.read_bytes()

    def test_explicit_independent_routes(self):
        blob=self.pack_document({"nodes":[{"id":7,"type":"splitter"},{"id":9,"type":"mixer","parameters":[.5,.5,1]}],
            "edges":[{"from":[0,1],"to":[7,1]}, {"from":[7,1],"to":[9,1]},
                     {"from":[7,2],"to":[9,2]}, {"from":[9,1],"to":[0,1]},
                     {"from":[0,2],"to":[0,2]}]})
        self.assertEqual(blob[14],5)
        self.assertEqual(struct.unpack_from("<HHfBBH",blob,40+12*28+2*12),(7,9,1.,1,1,0))

    def test_reject_bad_routes(self):
        nodes=[{"id":1,"type":"splitter"},{"id":2,"type":"mixer"}]
        for routes in (
            [{"from":[0,1],"to":[1,2]}], # mono input port overflow
            [{"from":[8,1],"to":[1,1]}], # unknown node
            [{"from":[0,1],"to":[1,1]},{"from":[0,2],"to":[1,1]}], # hidden summation
            [{"from":[1,1],"to":[2,1]},{"from":[2,1],"to":[1,1]}], # cycle
            [{"from":[0,1],"to":[1,1],"gain":float("nan")}],
        ):
            with self.subTest(routes=routes),self.assertRaises(ValueError):
                self.pack_document({"nodes":nodes,"edges":routes})
        self.assertEqual(self.pack_document({"nodes":nodes,"edges":[]})[14],0)

    def test_graph_matches_wire_layout_and_crc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "test.json"
            output = root / "test.sfg"
            source.write_text(json.dumps({
                "name": "Test",
                "nodes": [
                    {"type": "gain", "shape": "rounded", "parameters": [3]},
                    {"type": "mute", "enabled": False},
                ],
            }), encoding="utf-8")
            PACK.build_graph(source, output)
            blob = output.read_bytes()
            self.assertEqual(len(blob), 956)
            self.assertEqual(struct.unpack_from("<I", blob)[0], PACK.GRAPH_MAGIC)
            self.assertEqual(blob[13], 2)  # node_count
            self.assertEqual(blob[14], 6)  # explicit stereo edge_count
            self.assertEqual(struct.unpack_from("<I", blob, 952)[0],
                             zlib.crc32(blob[:952]))
            self.assertEqual(struct.unpack_from("<I", blob, 44)[0],
                             0x00010002)

    def test_effect_package_crc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "gain.json"
            output = root / "gain.sfx"
            source.write_text(json.dumps({"package_id": "0x12", "id": "gain"}),
                              encoding="utf-8")
            PACK.build_effect(source, output)
            blob = output.read_bytes()
            self.assertEqual(blob[:4], b"SFX1")
            stored_crc = struct.unpack_from("<I", blob, 24)[0]
            self.assertEqual(stored_crc,
                             zlib.crc32(blob[:24] + b"\0\0\0\0" + blob[28:]))

    def test_ready_card_contains_42_verified_effects(self):
        root = Path(__file__).parents[1] / "microSD-ready" / "SEEDFX"
        index = json.loads((root / "index.json").read_text(encoding="utf-8"))
        self.assertEqual(len(index["effects"]), 42)
        effect_types = set()
        for relative in index["effects"]:
            blob = (root / relative).read_bytes()
            self.assertEqual(blob[:4], b"SFX1")
            manifest_bytes = struct.unpack_from("<I", blob, 12)[0]
            self.assertEqual(len(blob), 28 + manifest_bytes)
            stored_crc = struct.unpack_from("<I", blob, 24)[0]
            self.assertEqual(stored_crc,
                             zlib.crc32(blob[:24] + b"\0\0\0\0" + blob[28:]))
            manifest = json.loads(blob[28:28 + manifest_bytes])
            effect_types.add(manifest["effect_type"])
        self.assertEqual(effect_types, set(range(1, 43)))
        self.assertEqual((root / index["autorun"]).stat().st_size,
                         PACK.GRAPH_BYTES)


if __name__ == "__main__":
    unittest.main()
