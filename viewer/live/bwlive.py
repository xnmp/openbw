#!/usr/bin/env python3
"""Turn a Brood War LAN game's network traffic into a live replay the viewer can follow.

    bwlive.py <net.pcap> --map <map.scx> -o <game.bwlv> [--follow]

Reads a pcap of one player's UDP traffic (Storm's LAN protocol on port 6112) and writes the
live replay format of ui/live_stream.h: the game's setup once it has started, then every
frame's commands as both players' turns arrive. With --follow it keeps reading the capture as
it grows, until `<net.pcap>.done` appears (the capture has stopped) or a player has left and
the traffic has gone quiet; the
viewer, `openbw-player --live <game.bwlv>`, follows the output the same way.

What the protocol gives, and what this relies on (measured on 1.16.1 LAN games between two
players, and checked by viewer/live/live_check.cpp against the replays the games saved):
  * Storm header (12 bytes, little-endian): checksum, length, sent seq, received seq, class,
    command, player, flags. Class 0 is Storm's own (join, names, game settings), class 1 is
    asynchronous (lobby room data, chat), class 2 is the synchronous turns. A resent message
    repeats its sent number; one copy counts.
  * Lobby: class 0 cmd 6/7 carry the host's and the joiner's names, cmd 8 the game's stat
    string (game name, speed, type, map checksum, tileset, host, map name), cmd 9 the 32-byte
    game settings that a replay header repeats as is; class 1 0x4A the map's size and slot
    owners; class 2 0x3E slot updates (slot, storm id, status, race, force).
  * Start: class 2 0x48 carries the seed (the replay's seed and start time) and 8 player bytes.
    Each game then puts players into start slots at random: the game's random number generator
    seeded with the seed draws once per lobby start slot, from the last to the first, and an
    occupied slot's draw picks its player's start among those left. This matches every one of
    the 142 recorded replays on 2- and 4-start maps.
  * Turns: one class 2 message per player per frame, in replay command form (no player byte;
    lengths from the command id), with a 0x37 sync and 0x05 keep-alives that replays omit.
    A message's frame is its sent number less that of the player's first 0x37 message, plus 1.
    Within a frame the game runs higher storm ids' commands first.
Only two-player games are handled; more players or a random race stop the decoder with a
message rather than guessing.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator

GAME_PORT = 6112
MAGIC, VERSION = 0x564C5742, 1
GAME_INFO_SIZE = 633
CMD_SYNC, CMD_KEEP_ALIVE, CMD_SEED, CMD_SLOT, CMD_ROOM = 0x37, 0x05, 0x48, 0x3E, 0x4A

# Command lengths, including the id byte (Brood War 1.16.1; jssuh, ShieldBattery).
_FIXED = {
    1: (0x05, 0x08, 0x10, 0x11, 0x18, 0x19, 0x1B, 0x1C, 0x1D, 0x27, 0x2A, 0x2E, 0x31, 0x33, 0x34,
        0x36, 0x38, 0x39, 0x3C, 0x54, 0x5A, 0x5B),
    2: (0x0F, 0x1A, 0x1E, 0x21, 0x22, 0x25, 0x26, 0x28, 0x2B, 0x2C, 0x2D, 0x30, 0x32, 0x3D, 0x42,
        0x43, 0x55, 0x57, 0x5F),
    3: (0x0D, 0x13, 0x1F, 0x20, 0x23, 0x29, 0x35, 0x41, 0x44, 0x45),
    4: (0x66,), 5: (0x0E, 0x12, 0x2F, 0x58, 0x62), 6: (0x3E,), 7: (0x37,), 8: (0x0C, 0x3F),
    10: (0x14, 0x56), 11: (0x15,), 12: (0x60,), 13: (0x61, 0x48), 18: (0x40,), 82: (0x5C,),
}
COMMAND_LENGTH = {c: n for n, cs in _FIXED.items() for c in cs}


class DecodeError(Exception):
    pass


def command_length(data: bytes, i: int) -> int:
    """The length of the command at data[i], with its id. Raises if it can't be sized or is cut."""
    c = data[i]
    if c in (0x09, 0x0A, 0x0B):
        n = 2 + 2 * data[i + 1] if i + 1 < len(data) else len(data) + 1
    elif c in (0x63, 0x64, 0x65):
        n = 2 + 4 * data[i + 1] if i + 1 < len(data) else len(data) + 1
    elif c in (0x06, 0x07):
        end = data.find(b"\0", i + 5)
        n = end + 1 - i if end >= 0 else len(data) + 1
    elif c in COMMAND_LENGTH:
        n = COMMAND_LENGTH[c]
    else:
        raise DecodeError(f"unknown command {c:#04x} at {i} in {data.hex(' ')}")
    if i + n > len(data):
        raise DecodeError(f"command {c:#04x} at {i} runs past the turn: {data.hex(' ')}")
    return n


def split_commands(data: bytes) -> list[bytes]:
    """A turn's bytes as its commands (each with its id). Raises on a command it can't size."""
    out, i = [], 0
    while i < len(data):
        n = command_length(data, i)
        out.append(data[i:i + n])
        i += n
    return out


class Lcg:
    """Brood War's random number generator."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFF

    def rand(self, lo: int, hi: int) -> int:
        self.state = (self.state * 22695477 + 1) & 0xFFFFFFFF
        return lo + ((((self.state >> 16) & 0x7FFF) * (hi - lo + 1)) >> 15)


def start_slots(seed: int, lobby: dict[int, int], start_count: int) -> dict[int, int]:
    """Where the game puts each player: {storm id: start slot}, from {storm id: lobby slot}."""
    rng, left, out = Lcg(seed), list(range(start_count)), {}
    by_slot = {s: p for p, s in lobby.items()}
    for slot in range(start_count - 1, -1, -1):
        k = rng.rand(0, len(left) - 1)
        if slot in by_slot:
            out[by_slot[slot]] = left.pop(k)
    return out


# ---- pcap ---------------------------------------------------------------------------------

class PcapReader:
    """Packets of a pcap file (Ethernet, IPv4 UDP) fed in as it grows: (time, src, dst, payload)."""

    def __init__(self) -> None:
        self.buf = b""
        self.header_done = False

    def feed(self, data: bytes) -> Iterator[tuple[float, str, str, bytes]]:
        self.buf += data
        p = 0
        if not self.header_done:
            if len(self.buf) < 24:
                return
            if struct.unpack_from("<I", self.buf)[0] != 0xA1B2C3D4:
                raise DecodeError("not a little-endian microsecond pcap")
            if struct.unpack_from("<I", self.buf, 20)[0] != 1:
                raise DecodeError("pcap is not Ethernet")
            self.header_done, p = True, 24
        while p + 16 <= len(self.buf):
            ts, us, incl, _ = struct.unpack_from("<IIII", self.buf, p)
            if p + 16 + incl > len(self.buf):
                break
            frame = self.buf[p + 16:p + 16 + incl]
            p += 16 + incl
            if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
                continue
            ihl = (frame[14] & 15) * 4
            u = 14 + ihl
            if len(frame) < u + 8:
                continue
            sport, dport, ulen = struct.unpack_from(">HHH", frame, u)
            if sport != GAME_PORT or dport != GAME_PORT:
                continue
            src, dst = ".".join(map(str, frame[26:30])), ".".join(map(str, frame[30:34]))
            yield ts + us / 1e6, src, dst, frame[u + 8:u + ulen]
        self.buf = self.buf[p:]


# ---- the game -----------------------------------------------------------------------------

@dataclass
class Slot:
    storm_id: int = 0xFF
    status: int = 0
    race: int = 0
    force: int = 0


@dataclass
class Player:
    first_sync: int | None = None   # sent number of its first 0x37 message
    turns: dict[int, list[bytes]] = field(default_factory=dict)  # frame -> commands
    last_frame: int = -1            # highest frame received
    contiguous: int = 0             # every frame below this has been received
    left: bool = False              # has left the game: no more turns


class Game:
    """Feeds on Storm messages; knows the setup once the game starts, then frames' commands."""

    def __init__(self) -> None:
        self.seen: set[tuple[str, str, int, int]] = set()
        self.names: dict[int, str] = {}
        self.stat: list[str] = []
        self.game_name = self.map_name = self.host_name = ""
        self.settings = b""
        self.map_w = self.map_h = self.tileset = 0
        self.owners: bytes = b""
        self.slots: dict[int, Slot] = {}
        self.map_size: int | None = None
        self.seed: int | None = None
        self.player_bytes = b"\x08" * 8
        self.host: str | None = None
        self.players: dict[int, Player] = {}
        self.addr_id: dict[str, int] = {}
        self.emitted = -1  # last frame handed out

    def message(self, src: str, dst: str, payload: bytes) -> None:
        if len(payload) < 12:
            return
        _, length, sent, _, cls, cmd, plr, flags = struct.unpack_from("<HHHHBBBB", payload)
        if flags in (1, 2):
            return  # Storm's sequence checks and resend requests: empty, and reuse a real number
        key = (src, dst, cls, sent)
        if key in self.seen:
            return
        self.seen.add(key)
        data = payload[12:length]
        if cls == 0:
            self._storm(src, cmd, plr, data)
        elif cls == 1:
            self._async(data)
        elif cls == 2:
            self._turn(src, plr, sent, data)

    def _storm(self, src: str, cmd: int, plr: int, data: bytes) -> None:
        if cmd == 6 and len(data) > 36:  # the host's player info
            self.host = src
            self.addr_id[src] = struct.unpack_from("<I", data, 4)[0]
            self.names[self.addr_id[src]] = data[36:].split(b"\0")[0].decode("latin-1")
        elif cmd == 7:  # the joiner's name
            self.names.setdefault(-1, data.split(b"\0")[0].decode("latin-1"))
        elif cmd == 8 and len(data) > 20:  # game info: name, then the stat string
            name, _, rest = data[20:].partition(b"\0")
            self.game_name = name.decode("latin-1")
            parts = rest.decode("latin-1").split("\r")
            self.stat = parts[0].split(",")
            self.map_name = parts[1] if len(parts) > 1 else ""
            self.host_name = self.stat[-1] if self.stat else ""
        elif cmd == 9 and len(data) >= 32:
            self.settings = data[:32]
        elif cmd == 0x0B and len(data) >= 5 and self.seed is not None:
            # A player leaves: the turn number it leaves at, and why. The game records it
            # as that player's Leave Game command, on that frame.
            sid = self.addr_id.get(src, plr)
            p = self.players.get(sid)
            if p is not None and p.first_sync is not None:
                frame = struct.unpack_from("<I", data)[0] - p.first_sync + 1
                if frame > self.emitted:
                    p.turns.setdefault(frame, []).append(bytes([0x57, data[4]]))
                    p.last_frame = max(p.last_frame, frame)
                    p.left = True

    def _async(self, data: bytes) -> None:
        if data[:1] == bytes([CMD_ROOM]) and len(data) >= 19:
            self.tileset, self.map_w, self.map_h = struct.unpack_from("<HHH", data, 1)
            self.owners = data[7:19]
        elif data[:1] == b"\x4f" and len(data) >= 8 and data[1] == 0x12:
            self.map_size = struct.unpack_from("<I", data, 5)[0]

    def _turn(self, src: str, plr: int, sent: int, data: bytes) -> None:
        if self.seed is None:
            if data[:1] == bytes([CMD_SEED]) and len(data) >= 13:
                self.seed = struct.unpack_from("<I", data, 1)[0]
                self.player_bytes = data[5:13]
                for sid in self.storm_ids():
                    self.players[sid] = Player()
                return
            try:
                lobby = split_commands(data)
            except DecodeError:
                return  # a lobby message this doesn't know; slot updates are what matter
            for c in lobby:
                if c[0] == CMD_SLOT:
                    self.slots[c[1]] = Slot(c[2], c[3], c[4], c[5])
            return
        sid = self.addr_id.get(src, plr)
        p = self.players.get(sid)
        if p is None:
            return
        if p.first_sync is None:
            if data[:1] != bytes([CMD_SYNC]):
                return  # keep-alives before its first turn
            p.first_sync = sent
        frame = sent - p.first_sync + 1
        if frame < 0 or frame in p.turns:
            return
        p.turns[frame] = [c for c in split_commands(data) if c[0] not in (CMD_SYNC, CMD_KEEP_ALIVE)]
        p.last_frame = max(p.last_frame, frame)
        while p.contiguous in p.turns or p.contiguous == 0:
            p.contiguous += 1

    def storm_ids(self) -> list[int]:
        return sorted(s.storm_id for s in self.slots.values() if s.storm_id < 8 and s.status != 0)

    def started(self) -> bool:
        return self.seed is not None

    def over(self) -> bool:
        """A player has left a two-player game: it is over for the other too."""
        return any(p.left for p in self.players.values())

    def game_info(self) -> bytes:
        """The 633-byte setup section of a replay, as the game would write it."""
        if self.seed is None:
            raise DecodeError("the game has not started")
        occupied = {s.storm_id: (i, s) for i, s in self.slots.items() if s.storm_id < 8 and s.status != 0}
        if len(occupied) != 2:
            raise DecodeError(f"only two-player games are handled, this one has {len(occupied)}")
        for i, s in occupied.values():
            if s.race > 2:
                raise DecodeError(f"lobby slot {i} has race {s.race} (random?), which can't be resolved yet")
        start_count = sum(1 for o in self.owners if o == 6) or 2
        where = start_slots(self.seed, {sid: i for sid, (i, _) in occupied.items()}, start_count)
        names = dict(self.names)
        joiner = [sid for sid in occupied if sid not in names]
        if joiner and -1 in names:
            names[joiner[0]] = names[-1]
        b = bytearray(GAME_INFO_SIZE)
        stat = self.stat + [""] * 12

        def put_str(off: int, n: int, s: str) -> None:
            raw = s.encode("latin-1", "replace")[:n - 1]
            b[off:off + len(raw)] = raw

        b[0] = 1  # Brood War
        b[7] = CMD_SEED
        struct.pack_into("<I", b, 8, self.seed)
        b[12:20] = self.player_bytes
        put_str(24, 24, self.host_name or names.get(0, ""))
        struct.pack_into("<HH", b, 52, self.map_w, self.map_h)
        b[56], b[57] = 1, len(occupied)
        b[58] = int(stat[3]) if stat[3].isdigit() else 3
        b[59] = 1
        b[60:64] = self.settings[:4] if len(self.settings) >= 4 else b"\x02\x00\x01\x00"
        try:
            struct.pack_into("<I", b, 64, int(stat[8], 16))
        except ValueError:
            pass
        struct.pack_into("<H", b, 68, self.tileset)
        b[70] = 1
        put_str(72, 25, self.game_name)
        put_str(97, 32, self.map_name)
        b[129:161] = (self.settings + bytes(32))[:32]
        by_slot = {where[sid]: (sid, s) for sid, (_, s) in occupied.items()}
        for i in range(12):
            o = 161 + 36 * i
            struct.pack_into("<I", b, o, i)
            if i in by_slot:
                sid, s = by_slot[i]
                struct.pack_into("<I", b, o + 4, sid)
                b[o + 8], b[o + 9], b[o + 10] = 2, s.race, 0
                put_str(o + 11, 25, names.get(sid, f"Player {sid + 1}"))
            else:
                struct.pack_into("<I", b, o + 4, 0xFFFFFFFF)
        for i in range(8):
            struct.pack_into("<I", b, 593 + 4 * i, i)  # colours: the game picks them at random; not sent
        return bytes(b)

    def ready_frames(self, final: bool = False) -> Iterator[tuple[int, bytes]]:
        """Frames whose commands are all known, in order, as (frame, replay-form commands)."""
        if not self.players:
            return
        playing = [p for p in self.players.values() if not p.left]
        if final or not playing:
            upto = max(p.last_frame for p in self.players.values())
        else:
            # Frames every player still in the game has sent (a player who left sends no more;
            # its leave frame is already among its turns).
            upto = min(p.contiguous - 1 for p in playing)
        for frame in range(self.emitted + 1, upto + 1):
            out = bytearray()
            for sid in sorted(self.players, reverse=True):
                for c in self.players[sid].turns.pop(frame, []):
                    out += bytes([sid]) + c
            self.emitted = frame
            yield frame, bytes(out)


# ---- output -------------------------------------------------------------------------------

def scenario_header(game_info: bytes, map_data: bytes) -> bytes:
    return struct.pack("<II", MAGIC, VERSION) + game_info + struct.pack("<I", len(map_data)) + map_data


def frame_records(frame: int, commands: bytes) -> bytes:
    """One frame's commands as records of at most 255 bytes, never splitting a command."""
    if not commands:
        return b""
    out, chunk, i = bytearray(), bytearray(), 0
    while i < len(commands):
        n = 1 + command_length(commands, i + 1)  # the next command, with its player byte
        leave = commands[i + 1] == 0x57  # a Leave Game gets a record of its own (live_check looks for it)
        if chunk and (len(chunk) + n > 255 or leave):
            out += struct.pack("<iB", frame, len(chunk)) + chunk
            chunk = bytearray()
        chunk += commands[i:i + n]
        i += n
        if leave and i < len(commands):
            out += struct.pack("<iB", frame, len(chunk)) + chunk
            chunk = bytearray()
    out += struct.pack("<iB", frame, len(chunk)) + chunk
    return bytes(out)


def complete_record(frame: int) -> bytes:
    return struct.pack("<iB", frame, 0)


def end_record() -> bytes:
    return struct.pack("<iB", -1, 0)


def run(pcap: str, map_path: str, out_path: str, follow: bool, poll: float = 0.05,
        idle_timeout: float = 600.0, log: BinaryIO | None = None) -> int:
    with open(map_path, "rb") as f:
        map_data = f.read()
    game, reader = Game(), PcapReader()
    out = open(out_path + ".part", "wb")
    header_written, last_growth, pos = False, time.monotonic(), 0
    done_marker = pcap + ".done"
    emitted_frames = 0

    def say(msg: str) -> None:
        print(f"bwlive: {msg}", file=sys.stderr, flush=True)

    while True:
        # Done when the capture is (it stopped, or this is a whole file), or a player left and
        # the rest has gone quiet for a few seconds.
        finished = os.path.exists(done_marker) or not follow or (game.over() and time.monotonic() - last_growth > 3)
        try:
            with open(pcap, "rb") as f:
                f.seek(pos)
                data = f.read()
        except FileNotFoundError:
            data = b""
        if data:
            pos += len(data)
            last_growth = time.monotonic()
            for _, src, dst, payload in reader.feed(data):
                game.message(src, dst, payload)
        if game.started() and not header_written:
            if game.map_size is not None and game.map_size != len(map_data):
                say(f"warning: the game's map is {game.map_size} bytes, {map_path} is {len(map_data)}")
            out.write(scenario_header(game.game_info(), map_data))
            out.flush()
            out.close()
            os.replace(out_path + ".part", out_path)  # the viewer only ever sees a whole header
            out = open(out_path, "ab")
            header_written = True
            say(f"game started: seed {game.seed}, players {game.storm_ids()} {game.names}")
        if header_written:
            chunk = bytearray()
            newest = None
            for frame, cmds in game.ready_frames(final=finished):
                chunk += frame_records(frame, cmds)
                newest = emitted_frames = frame
            if newest is not None:
                chunk += complete_record(newest)  # frames through `newest` are complete
            if finished:
                chunk += end_record()
            if chunk:
                out.write(chunk)
                out.flush()
        if finished:
            break
        if time.monotonic() - last_growth > idle_timeout:
            say("capture stopped growing; ending")
            follow = False
            continue
        time.sleep(poll)
    out.close()
    if not header_written:
        say("the game never started in this capture")
        return 1
    say(f"done: {emitted_frames + 1} frames")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--map", required=True, help="the game's map file (.scx/.scm)")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--follow", action="store_true", help="keep reading the capture as it grows")
    a = ap.parse_args()
    try:
        return run(a.pcap, a.map, a.out, a.follow)
    except DecodeError as e:
        print(f"bwlive: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
