"""Behaviour tests for bwlive.py's pure parts. Run: python3 -m unittest viewer/live/test_bwlive.py
(from the repository root; stdlib only). The decoder as a whole is checked against real games'
replays by live_check.cpp."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bwlive  # noqa: E402


class StartSlots(unittest.TestCase):
    # Seeds and where the game put the host (storm id 0, lobby slot 0) and the joiner
    # (storm id 1, lobby slot 1), from replays the games saved.
    def test_four_start_map(self):
        self.assertEqual(bwlive.start_slots(1793776509, {0: 0, 1: 1}, 4), {0: 3, 1: 2})
        self.assertEqual(bwlive.start_slots(1793790709, {0: 0, 1: 1}, 4), {0: 0, 1: 1})
        self.assertEqual(bwlive.start_slots(1793781302, {0: 0, 1: 1}, 4), {0: 3, 1: 0})

    def test_two_start_map(self):
        self.assertEqual(bwlive.start_slots(1794124182, {0: 0, 1: 1}, 2), {0: 1, 1: 0})
        self.assertEqual(bwlive.start_slots(1794124420, {0: 0, 1: 1}, 2), {0: 0, 1: 1})

    def test_every_player_gets_a_distinct_start(self):
        for seed in range(0, 2**32, 2**32 // 997):
            got = bwlive.start_slots(seed, {0: 0, 1: 1}, 4)
            self.assertEqual(sorted(got), [0, 1])
            self.assertEqual(len(set(got.values())), 2)
            self.assertTrue(all(0 <= s < 4 for s in got.values()))


class Commands(unittest.TestCase):
    def test_split(self):
        turn = bytes.fromhex("37 92 2a 8b 00 00 0f 09 01 46 0e 14 80 00 50 0a 94 0e b0 00 00")
        self.assertEqual([c[0] for c in bwlive.split_commands(turn)], [0x37, 0x09, 0x14])
        self.assertEqual(bwlive.split_commands(b""), [])
        # A selection of three units is 2 + 2 * 3 bytes.
        self.assertEqual(len(bwlive.split_commands(bytes.fromhex("09 03 01 00 02 00 03 00"))[0]), 8)

    def test_malformed(self):
        with self.assertRaises(bwlive.DecodeError):
            bwlive.split_commands(b"\xee")  # no such command
        with self.assertRaises(bwlive.DecodeError):
            bwlive.split_commands(bytes.fromhex("14 80 00"))  # a right-click cut short
        with self.assertRaises(bwlive.DecodeError):
            bwlive.split_commands(bytes.fromhex("09 05 01 00"))  # a selection cut short
        with self.assertRaises(bwlive.DecodeError):
            bwlive.split_commands(b"\x09")  # no count


class Records(unittest.TestCase):
    @staticmethod
    def parse(data):
        out, p = [], 0
        while p < len(data):
            f, n = struct.unpack_from("<iB", data, p)
            out.append((f, data[p + 5:p + 5 + n]))
            p += 5 + n
        return out

    def test_no_commands_no_record(self):
        self.assertEqual(bwlive.frame_records(7, b""), b"")

    def test_long_frame_splits_between_commands(self):
        click = bytes.fromhex("14 80 00 50 0a 94 0e b0 00 00")
        cmds = b"".join(bytes([i % 2]) + click for i in range(60))  # 660 bytes
        recs = self.parse(bwlive.frame_records(3, cmds))
        self.assertTrue(all(f == 3 and 0 < len(b) <= 255 for f, b in recs))
        self.assertEqual(b"".join(b for _, b in recs), cmds)
        self.assertTrue(all(len(b) % 11 == 0 for _, b in recs))  # whole commands only

    def test_leave_gets_its_own_record(self):
        cmds = bytes.fromhex("01 57 01") + bytes.fromhex("00 1a 03")
        recs = self.parse(bwlive.frame_records(9, cmds))
        self.assertEqual(recs, [(9, bytes.fromhex("01 57 01")), (9, bytes.fromhex("00 1a 03"))])


class Pcap(unittest.TestCase):
    @staticmethod
    def frame(payload, sport=6112, dport=6112):
        ip = bytes([0x45, 0, 0, 0, 0, 0, 0, 0, 64, 17, 0, 0, 172, 17, 0, 2, 172, 17, 0, 3])
        udp = struct.pack(">HHHH", sport, dport, 8 + len(payload), 0)
        eth = b"\0" * 12 + b"\x08\x00"
        return eth + ip + udp + payload

    def file(self, frames):
        out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
        for f in frames:
            out += struct.pack("<IIII", 1, 2, len(f), len(f)) + f
        return out

    def test_fed_in_pieces(self):
        data = self.file([self.frame(b"one"), self.frame(b"x", sport=53), self.frame(b"three")])
        r, got = bwlive.PcapReader(), []
        for i in range(len(data)):
            got += [p for _, _, _, p in r.feed(data[i:i + 1])]
        self.assertEqual(got, [b"one", b"three"])  # only the game port

    def test_not_a_pcap(self):
        with self.assertRaises(bwlive.DecodeError):
            list(bwlive.PcapReader().feed(b"\0" * 24))


if __name__ == "__main__":
    unittest.main()
