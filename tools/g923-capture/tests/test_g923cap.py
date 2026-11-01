import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import fakewheel as fw  # noqa: E402
from g923cap import analyse, descriptors, pcap, probe, truforce  # noqa: E402


class Temp(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class PcapTest(Temp):
    def test_round_trip_and_truncated_file(self):
        c = fw.Capture()
        c.descriptors()
        c.out(0x03, fw.tf_request(5, 1, b"\x02"))
        c.inn(0x83, fw.answer(fw.tf_request(5, 1, b"\x02"), 0, 0)[0])
        p = self.dir / "a.pcap"
        pcap.write_pcap(str(p), c.frames)
        tr = pcap.transfers(pcap.read_urbs(str(p)))
        kinds = [t.kind for t in tr]
        self.assertEqual(kinds.count("control"), 5)
        self.assertEqual(kinds.count("out"), 1)
        self.assertEqual(kinds.count("in"), 1)
        data = p.read_bytes()
        p.write_bytes(data[:-10])   # cut in the last packet
        tr2 = pcap.transfers(pcap.read_urbs(str(p)))
        self.assertEqual(len(tr2), len(tr) - 1)

    def test_pcapng(self):
        c = fw.Capture()
        c.descriptors()
        p = self.dir / "a.pcapng"
        with open(p, "wb") as f:
            shb = struct.pack("<IIIHHq", 0x0A0D0D0A, 28, 0x1A2B3C4D, 1, 0, -1) + struct.pack("<I", 28)
            f.write(shb)
            idb = struct.pack("<IIHHI", 1, 20, pcap.LINKTYPE_USBPCAP, 0, 65535) + struct.pack("<I", 20)
            f.write(idb)
            for ts, frame in c.frames:
                pad = (4 - len(frame) % 4) % 4
                total = 32 + len(frame) + pad
                us = int(ts * 1e6)
                f.write(struct.pack("<IIIIIII", 6, total, 0, us >> 32, us & 0xFFFFFFFF, len(frame), len(frame)))
                f.write(frame + b"\x00" * pad + struct.pack("<I", total))
        tr = pcap.transfers(pcap.read_urbs(str(p)))
        self.assertEqual(len(tr), 5)
        self.assertAlmostEqual(tr[0].ts, c.frames[1][0], places=5)


class DescriptorTest(unittest.TestCase):
    def test_report_descriptors(self):
        tfc = descriptors.parse_report_descriptor(fw.RD_TF)
        self.assertEqual(tfc[0].usage_page, 0xFFFD)
        self.assertEqual(tfc[0].usage, 0xFD01)
        self.assertEqual(tfc[0].report_bytes(1, 0), 64)
        self.assertEqual(tfc[0].report_bytes(1, 1), 64)
        hp = descriptors.parse_report_descriptor(fw.RD_HIDPP)
        self.assertEqual([c.usage_page for c in hp], [0xFF43, 0xFF43])
        self.assertEqual(hp[0].report_bytes(0x11, 0), 20)
        self.assertEqual(hp[1].report_bytes(0x12, 1), 64)

    def test_configuration(self):
        ifs = descriptors.parse_configuration(fw.configuration())
        self.assertEqual([i.number for i in ifs], [0, 1, 2])
        self.assertEqual(ifs[2].endpoints[0].address, 0x83)
        self.assertEqual(ifs[2].hid_report_length, len(fw.RD_TF))


class AnalyseTest(Temp):
    def make_s2(self, params=fw.PARAMS):
        folder = self.dir / "S2-acc"
        folder.mkdir()
        fw.s2_like(str(folder / "capture.pcap"), str(folder / "notes.txt"), params=params)
        (folder / "wheelbri-shim-2027-01-15-01.log").write_text(
            "[2027-01-15 10:00:01.000] [info] [shim.logi] logiTrueForceSetTorqueKF #1: rcx=0 | rax=1\n"
            "[2027-01-15 10:00:01.001] [info] [shim.logi] logiTrueForceSetTorqueKF #2: rcx=0 | rax=1\n")
        return folder

    def repo(self):
        repo = self.dir / "repo"
        (repo / "core" / "src").mkdir(parents=True)
        (repo / "core" / "src" / "TrueForce.cpp").write_text(
            "float TrueForceDevice::parameter(const std::uint8_t index) {\n switch (index) {\n"
            "        case 0x00: return 2.0F;\n        case 0x08: return 1.1F;   // note\n"
            "        case 0x36: return -1.0F;\n        default: return 0.0F;\n }\n}\n")
        return repo

    def test_s2_passes_and_finds_values(self):
        folder = self.make_s2()
        results = analyse.decode(folder, self.repo())
        r = results[0]
        for ok, text in analyse.checks(r):
            self.assertTrue(ok, text)
        params = r["truforce"]["parameters"]
        self.assertEqual(len(params), 0x30)
        self.assertAlmostEqual(params["0x08"]["value"], 1.1, places=5)
        self.assertAlmostEqual(params["0x06"]["value"], 123.0)
        self.assertTrue(all(p["well_formed"] for p in params.values()))
        st = r["truforce"]["status"]
        self.assertAlmostEqual(list(st["0x02"]["torque1_nm"])[0], 2.2, places=3)
        self.assertEqual(list(st["0x02"]["count_range"])[0], fw.COUNT_RANGE)
        holds = {h["label"]: h for h in r["truforce"]["holds"]}
        self.assertAlmostEqual(holds["right90"]["counts_per_turn"], 65536, delta=5)
        self.assertAlmostEqual(holds["left180"]["counts_per_turn"], 65536, delta=5)
        pairs = r["truforce"]["pairs"]
        self.assertEqual(pairs["0x0E"]["answer_types"], {"0x11": 1})
        self.assertEqual(pairs["0x09"]["answered"], 0)
        self.assertEqual(r["truforce"]["stream"]["count"], 500)
        self.assertTrue((folder / "stream.tsv").exists())
        summary = (folder / "summary.md").read_text()
        self.assertIn("| 0x06 | **123** | 0 |", summary)   # differs from WheelBri: bold
        self.assertIn("| 0x08 | 1.1 | 1.1 |", summary)
        self.assertIn("logiTrueForceSetTorqueKF x2", summary)
        json.loads((folder / "analysis.json").read_text())

    def test_borrows_report_descriptors_and_kinds_by_majority(self):
        folder = self.make_s2()
        s1 = self.dir / "S1-plugin"
        s1.mkdir()
        fw.s1_like(str(s1 / "capture.pcap"))
        analyse.decode(s1)
        self.assertTrue((self.dir / "descriptors-046D-C26E.json").exists())
        r = analyse.decode(folder)[0]
        self.assertEqual(r.get("report_descriptors_from"), "descriptors-046D-C26E.json")
        kinds = {i["number"]: i["kind"] for i in r["target"]["interfaces"]}
        self.assertEqual(kinds, {0: "input", 1: "hidpp", 2: "truforce"})

    def test_kind_by_majority_without_descriptors(self):
        c = fw.Capture()
        c.descriptors(report_descriptors=False)
        for k in range(30):   # game reports on interface 0, one of them shaped like HID++ long
            c.inn(0x81, bytes([0x11]) + bytes(19) if k == 3 else bytes(13))
        folder = self.dir / "S5-x"
        folder.mkdir()
        pcap.write_pcap(str(folder / "capture.pcap"), c.frames)
        r = analyse.decode(folder)[0]
        self.assertEqual(r["interface_kinds"], {"0": "input"})
        self.assertEqual(r["hidpp"]["requests"], 0)
        self.assertEqual(r["hidpp"]["unsolicited"]["count"], 0)

    def test_holds_out_of_order_fail(self):
        folder = self.make_s2()
        notes = (folder / "notes.txt").read_text().replace("right90|90", "right90|-45")
        (folder / "notes.txt").write_text(notes)
        r = analyse.decode(folder)[0]
        bad = [t for ok, t in analyse.checks(r) if not ok]
        self.assertTrue(any("held angles" in t for t in bad), bad)

    def test_s1_hidpp_and_descriptors(self):
        folder = self.dir / "S1-plugin"
        folder.mkdir()
        fw.s1_like(str(folder / "capture.pcap"))
        r = analyse.decode(folder)[0]
        for ok, text in analyse.checks(r):
            self.assertTrue(ok, text)
        hp = r["hidpp"]
        self.assertEqual(hp["feature_map"]["4"], 0x8123)
        self.assertEqual(hp["name"], "G923 Racing Wheel for Xbox One and PC")
        self.assertEqual(hp["firmware"][0]["prefix"], "U1 ")
        kinds = {i["number"]: i["kind"] for i in r["target"]["interfaces"]}
        self.assertEqual(kinds, {0: "input", 1: "hidpp", 2: "truforce"})

    def test_day_report(self):
        self.make_s2()
        s1 = self.dir / "S1-plugin"
        s1.mkdir()
        fw.s1_like(str(s1 / "capture.pcap"))
        analyse.decode(self.dir, self.repo())
        report = (self.dir / "report.md").read_text()
        self.assertIn("S1-plugin", report)
        self.assertIn("4: 0x8123 ForceFeedback", report)
        self.assertIn("| 0x06 | **123** |", report)

    def test_no_wheel(self):
        folder = self.dir / "S2-x"
        folder.mkdir()
        c = fw.Capture()
        c.out(0x01, b"\x05\x00")
        pcap.write_pcap(str(folder / "c.pcap"), c.frames)
        r = analyse.decode(folder)[0]
        self.assertFalse(analyse.checks(r)[0][0])

    def test_locate(self):
        a, b = self.dir / "hub1.pcap", self.dir / "hub2.pcap"
        pcap.write_pcap(str(a), [])
        c = fw.Capture(bus=2, address=9)
        c.descriptors(False)
        pcap.write_pcap(str(b), c.frames)
        found = analyse.locate([a, b])
        self.assertEqual(found, [{"file": str(b), "bus": 2, "address": 9, "product": "C26E"}])


class KeepTest(Temp):
    def test_keeps_only_the_wheel(self):
        folder = self.dir / "S1-x"
        folder.mkdir()
        wheel = fw.Capture(bus=1, address=7)
        wheel.descriptors()
        keyboard = fw.Capture(bus=1, address=3)
        keyboard.inn(0x81, b"\x00\x00\x04\x00\x00\x00\x00\x00")
        frames = sorted(wheel.frames + keyboard.frames, key=lambda f: f[0])
        pcap.write_pcap(str(folder / "raw-USBPcap1.pcap"), frames)
        pcap.write_pcap(str(folder / "raw-USBPcap2.pcap"), keyboard.frames)
        said = analyse.keep_wheel_only(folder)
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["capture.pcap"])
        self.assertTrue(any("deleted raw-USBPcap2.pcap" in s for s in said), said)
        tr = pcap.transfers(pcap.read_urbs(str(folder / "capture.pcap")))
        self.assertTrue(all(t.device == 7 for t in tr))
        self.assertEqual(len(tr), 5)


    def test_keeps_everything_when_no_wheel_is_found(self):
        folder = self.dir / "S1-y"
        folder.mkdir()
        keyboard = fw.Capture(bus=1, address=3)
        keyboard.inn(0x81, b"\x00\x00\x04\x00\x00\x00\x00\x00")
        pcap.write_pcap(str(folder / "raw-USBPcap1.pcap"), keyboard.frames)
        said = analyse.keep_wheel_only(folder)
        self.assertEqual([p.name for p in folder.iterdir()], ["raw-USBPcap1.pcap"])
        self.assertIn("all kept", said[0])


class RehearsalTest(Temp):
    def tearDown(self):
        os.environ.pop("G923_TARGET", None)
        super().tearDown()

    def test_another_wheel(self):
        folder = self.dir / "S1-plugin-ghub"
        folder.mkdir()
        c = fw.Capture(bus=2, address=4)
        moza = bytearray(fw.DEVICE)
        moza[8:12] = bytes([0x6E, 0x34, 0x04, 0x00])   # 346E:0004
        c.control_in(bytes([0x80, 6, 0, 1, 0, 0, 18, 0]), bytes(moza))
        cfg = fw.configuration()
        c.control_in(bytes([0x80, 6, 0, 2, 0, 0]) + struct.pack("<H", len(cfg)), cfg)
        for i, rd in enumerate((fw.RD_GAME, fw.RD_HIDPP, fw.RD_TF)):
            c.control_in(bytes([0x81, 6, 0, 0x22, i, 0]) + struct.pack("<H", len(rd)), rd)
        c.inn(0x81, bytes([1]) + bytes(10))
        pcap.write_pcap(str(folder / "raw-USBPcap2.pcap"), c.frames)
        self.assertIn("all kept", analyse.keep_wheel_only(folder)[0])   # not a G923: nothing deleted
        pcap.write_pcap(str(folder / "raw-USBPcap2.pcap"), c.frames)
        os.environ["G923_TARGET"] = "346E"
        self.assertTrue(analyse.keep_wheel_only(folder)[0].startswith("kept"))
        r = analyse.decode(folder)[0]
        cs = analyse.checks(r)
        self.assertIn("REHEARSAL", cs[0][1])
        for ok, text in cs:
            self.assertTrue(ok, text)


class ReEnumerationTest(Temp):
    def test_keep_keeps_every_address_of_the_wheel(self):
        folder = self.dir / "S3-x"
        folder.mkdir()
        first = fw.Capture(bus=1, address=7)
        first.descriptors(False)
        first.inn(0x81, bytes(13))
        second = fw.Capture(t0=first.t + 1, bus=1, address=9)   # the wheel again, after a reset
        second.descriptors(False)
        second.inn(0x81, bytes(13))
        keyboard = fw.Capture(bus=1, address=3)
        keyboard.inn(0x81, bytes(8))
        pcap.write_pcap(str(folder / "raw-USBPcap1.pcap"), sorted(first.frames + second.frames + keyboard.frames))
        said = analyse.keep_wheel_only(folder)
        self.assertIn("bus 1 address 7, bus 1 address 9", said[0])
        devices = {t.device for t in pcap.transfers(pcap.read_urbs(str(folder / "capture.pcap")))}
        self.assertEqual(devices, {7, 9})

    def test_hidpp_sharing_the_game_interface(self):
        c = fw.Capture()
        c.descriptors(report_descriptors=False)
        for k in range(20):
            c.inn(0x81, bytes(13))
        q = fw.hidpp_long(0, 0, 1, b"\x81\x23")
        c.out(0x01, q)
        c.inn(0x81, fw.hidpp_answer(q))
        folder = self.dir / "S5-y"
        folder.mkdir()
        pcap.write_pcap(str(folder / "capture.pcap"), c.frames)
        r = analyse.decode(folder)[0]
        self.assertEqual(r["interface_kinds"], {"0": "input+hidpp"})
        self.assertEqual(r["hidpp"]["requests"], 1)
        self.assertEqual(r["hidpp"]["answered"], 1)


class MixedInterfaceTest(Temp):
    def test_game_hidpp_and_truforce_on_one_interface(self):
        from g923cap import descriptors as d
        rd = fw.RD_GAME + fw.RD_HIDPP + fw.RD_TF.replace(bytes([0x85, 0x01]), bytes([0x85, 0x05]))
        cols = d.parse_report_descriptor(rd)
        self.assertEqual([c.usage_page for c in cols], [0x01, 0xFF43, 0xFF43, 0xFFFD])
        dev = analyse.Device(1, 7)
        dev.report_descriptors[0] = rd
        self.assertEqual(dev.kind_of_interface(0), "mixed")
        self.assertEqual(dev.kind_of_report(0, 0x01), "input")
        self.assertEqual(dev.kind_of_report(0, 0x11), "hidpp")
        self.assertEqual(dev.kind_of_report(0, 0x05), "truforce")
        r = analyse.Report(0, "in", 0, bytes([0x11, 0xFF, 0, 0]) + bytes(16), 1, "interrupt")
        self.assertEqual(analyse.classify(dev, r), "hidpp")
        r = analyse.Report(0, "in", 0, bytes([0x01]) + bytes(12), 2, "interrupt")
        self.assertEqual(analyse.classify(dev, r), "input")


class ProbeTest(unittest.TestCase):
    def test_truforce_parameters(self):
        t = fw.FakeTransport("tf")
        res = probe.probe_truforce(t, indexes=range(0x40))
        self.assertEqual(len(res["parameters"]), 0x40)
        self.assertAlmostEqual(res["parameters"]["0x0C"]["value"], 47.1239, places=3)
        self.assertFalse(res["sealed_requests"])
        self.assertIsNone(res["slots"])   # 0x07 is not sent
        self.assertEqual(res["other_reports"].get("0x02"), 2 * 0x40)
        self.assertTrue(all(w[4] in probe.SAFE_TRUFORCE_TYPES for w in t.written))

    def test_truforce_needs_seal(self):
        t = fw.FakeTransport("tf", needs_seal=True)
        res = probe.probe_truforce(t, indexes=range(4))
        self.assertTrue(res["sealed_requests"])
        self.assertEqual(len(res["parameters"]), 4)

    def test_hidpp(self):
        t = fw.FakeTransport("hidpp")
        res = probe.probe_hidpp(t)
        self.assertEqual(res["protocol"], "4.2")
        self.assertEqual(res["features"]["0x8123"]["index"], 4)
        self.assertEqual(res["name"], "G923 Racing Wheel for Xbox One and PC")
        self.assertEqual(res["firmware"][0]["prefix"], "U1 ")
        for w in t.written:
            fid = fw.FEATURES[w[2]]
            self.assertIn((fid, w[3] >> 4), probe.SAFE_HIDPP)
            self.assertEqual(w[3] & 0x0F, probe.SOFTWARE_ID)

    def test_only_reads_allowed(self):
        self.assertEqual(probe.SAFE_TRUFORCE_TYPES, {0x05})
        for t in (0x01, 0x03, 0x04, 0x06, 0x07, 0x0C, 0x0E):
            with self.assertRaises(probe.Unsafe):
                probe.truforce_request(t, 1, b"", 64, False)
        with self.assertRaises(probe.Unsafe):
            probe.hidpp_request(4, 0x8123, 1, b"", 20)
        self.assertEqual({f for f, _ in probe.SAFE_HIDPP}, {0x0000, 0x0001, 0x0003, 0x0005})

    def test_markdown(self):
        t = fw.FakeTransport("tf")
        res = {"truforce": probe.probe_truforce(t, indexes=range(3)), "collections": {}}
        md = probe.markdown(res, {0: 2.0, 1: 9.0})
        self.assertIn("| 0x01 | **1** | 9 |", md)


class WheelBriParametersTest(unittest.TestCase):
    def test_reads_the_real_source(self):
        repo = Path(os.environ.get("WHEELBRI_REPO", HERE.parents[2]))
        values = truforce.wheelbri_parameters(repo)
        if not values:
            self.skipTest(f"no core/src/TrueForce.cpp under {repo}")
        self.assertEqual(values[0x08], 1.1)
        self.assertEqual(values[0x36], -1.0)
        self.assertGreaterEqual(len(values), 20)


if __name__ == "__main__":
    unittest.main()
