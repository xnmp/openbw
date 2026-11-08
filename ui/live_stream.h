#ifndef OPENBW_LIVE_STREAM_H
#define OPENBW_LIVE_STREAM_H

// A game being played, as a growing file the viewer follows: a live replay. A decoder that
// watches the game (viewer/live/bwlive.py reads the players' LAN traffic) writes the game's
// setup once and then each frame's commands as the players send them; the viewer simulates
// up to the last frame it has, and waits for more. Nothing here knows about networking.
//
// Layout (little-endian):
//   "BWLV" (u32 0x564c5742), u32 version (1)
//   game info: 633 bytes, as in a replay's header section (seed, slots, races, map name...)
//   u32 n, n bytes: the map: a scenario.chk, or the .scx/.scm (MPQ) holding one
//   records, appended as the game goes:
//     i32 frame >= 0, u8 n > 0, n bytes: commands for that frame in replay form,
//       (u8 player, command)... A frame may have several such records, in order.
//     i32 frame >= 0, u8 0: every command up to and including that frame is known (a frame
//       is not complete until a later marker says so: its commands may span records).
//     i32 -1, u8 0: the game is over (no more records).
//   Frames never go back, and no commands come for a frame already marked complete.
//
// `reader` parses the file incrementally from bytes as they appear (feed whatever has been
// read since last time; a record cut short is kept until the rest arrives). Pure, so it can
// be tested.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace live_stream {

constexpr uint32_t magic = 0x564c5742;
constexpr uint32_t version = 1;
constexpr size_t game_info_size = 633;
constexpr size_t max_map_size = 64u << 20;  // far beyond any map; guards against a bad length

class reader {
public:
	// Bytes read from the file since the last call.
	void feed(const uint8_t* data, size_t n) {
		if (failed_) return;
		pending.insert(pending.end(), data, data + n);
		parse();
	}

	bool has_setup() const { return setup_done; }
	const std::array<uint8_t, game_info_size>& game_info() const { return info; }
	const std::vector<uint8_t>& map() const { return map_data; }

	// Replay-form action records parsed since the last call (moved out): append them to the
	// replay's action data.
	std::vector<uint8_t> take_actions() {
		std::vector<uint8_t> r;
		r.swap(actions);
		return r;
	}

	// The last frame whose commands are all known (-1: none yet).
	int known_frame() const { return known; }
	bool ended() const { return ended_; }
	bool failed() const { return failed_; }
	const std::string& error() const { return error_; }

private:
	std::vector<uint8_t> pending;
	size_t pos = 0;
	bool header_done = false, setup_done = false, ended_ = false, failed_ = false;
	std::array<uint8_t, game_info_size> info{};
	std::vector<uint8_t> map_data, actions;
	int known = -1, last_frame = -1;
	std::string error_;

	size_t left() const { return pending.size() - pos; }
	uint32_t u32_at(size_t at) const {
		return (uint32_t)pending[at] | (uint32_t)pending[at + 1] << 8 | (uint32_t)pending[at + 2] << 16 | (uint32_t)pending[at + 3] << 24;
	}
	void fail(std::string why) {
		failed_ = true;
		error_ = std::move(why);
		pending.clear();
		pos = 0;
	}

	void parse() {
		if (!header_done) {
			if (left() < 8 + game_info_size) return;
			if (u32_at(pos) != magic) return fail("not a live replay (bad magic)");
			if (u32_at(pos + 4) != version) return fail("unsupported live replay version " + std::to_string(u32_at(pos + 4)));
			std::memcpy(info.data(), pending.data() + pos + 8, game_info_size);
			pos += 8 + game_info_size;
			header_done = true;
		}
		if (!setup_done) {
			if (left() < 4) return;
			uint32_t n = u32_at(pos);
			if (n == 0 || n > max_map_size) return fail("bad map size " + std::to_string(n));
			if (left() < 4 + (size_t)n) return;
			map_data.assign(pending.begin() + pos + 4, pending.begin() + pos + 4 + n);
			pos += 4 + n;
			setup_done = true;
		}
		while (!ended_ && left() >= 5) {
			int32_t frame = (int32_t)u32_at(pos);
			size_t n = pending[pos + 4];
			if (frame == -1) {
				if (n != 0) return fail("bad end record");
				pos += 5;
				ended_ = true;
				known = std::max(known, last_frame);  // at the end, every frame is complete
				break;
			}
			if (frame < 0) return fail("bad frame " + std::to_string(frame));
			if (frame < last_frame) return fail("frame went back from " + std::to_string(last_frame) + " to " + std::to_string(frame));
			if (n && frame <= known) return fail("commands for frame " + std::to_string(frame) + ", already complete");
			if (left() < 5 + n) break;
			if (n) actions.insert(actions.end(), pending.begin() + pos, pending.begin() + pos + 5 + n);
			else known = frame;
			last_frame = frame;
			pos += 5 + n;
		}
		// Drop what has been parsed, now and then, so a long game doesn't keep its whole file.
		if (pos > 65536 || pos == pending.size()) {
			pending.erase(pending.begin(), pending.begin() + pos);
			pos = 0;
		}
	}
};

}  // namespace live_stream

#endif
