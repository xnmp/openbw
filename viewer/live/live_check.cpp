// Checks live replays (ui/live_stream.h) against the replay the game saved, headless.
//
//   live_check info <replay.rep>
//       Print the replay's game info (the 633-byte setup section) and its first commands.
//   live_check compare <replay.rep> <game.bwlv> <data-dir> [chunk-bytes]
//       Simulate both, frame by frame, and report the first frame where the game states
//       differ (units, positions, hit points, resources, the random number state), or that
//       they agree up to the last frame the live replay has. With chunk-bytes, the live
//       replay is read in pieces of that size and followed as the viewer follows a game.
//
// Build: c++ -std=gnu++14 -O2 -I. -Iui viewer/live/live_check.cpp -o live_check
#include "bwgame.h"
#include "replay.h"
#include "live_stream.h"
#include "live_replay.h"

#include <cstdio>
#include <fstream>
#include <iterator>

using namespace bwgame;

namespace {

std::vector<uint8_t> read_all(const char* path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error(std::string("cannot read ") + path);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

struct sections {
	std::array<uint8_t, 633> info;
	std::vector<uint8_t> actions, map;
};

sections read_replay(const char* path) {
	auto file_r = data_loading::file_reader<>(path);
	auto r = data_loading::make_replay_file_reader(file_r);
	sections s;
	if (r.get<uint32_t>() != 0x53526572) throw std::runtime_error("not a replay");
	r.get_bytes(s.info.data(), s.info.size());
	s.actions.resize(r.get<uint32_t>());
	r.get_bytes(s.actions.data(), s.actions.size());
	s.map.resize(r.get<uint32_t>());
	r.get_bytes(s.map.data(), s.map.size());
	return s;
}

using live_replay::scenario_chk;

std::vector<uint8_t> raw_replay(const std::array<uint8_t, 633>& info, const std::vector<uint8_t>& actions, const std::vector<uint8_t>& chk) {
	uint32_t frames = (uint32_t)(info[1] | info[2] << 8 | info[3] << 16 | (uint32_t)info[4] << 24);
	return live_replay::raw_replay(info, frames, actions, chk);
}

uint64_t state_hash(const state& st) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
	mix(st.lcg_rand_state);
	for (int p = 0; p != 12; ++p) {
		mix(st.current_minerals[p]);
		mix(st.current_gas[p]);
		for (const unit_t* u : ptr(st.player_units[p])) {
			mix((uint64_t)u->unit_type->id);
			mix((uint64_t)(uint16_t)u->sprite->position.x << 16 | (uint16_t)u->sprite->position.y);
			mix((uint64_t)u->hp.raw_value);
			mix((uint64_t)u->order_type->id);
		}
	}
	return h;
}

int count_units(const state& st, int p) {
	int n = 0;
	for (const unit_t* u : ptr(st.player_units[p])) (void)u, ++n;
	return n;
}

void print_info(const std::array<uint8_t, 633>& b) {
	auto u8 = [&](size_t o) { return (unsigned)b[o]; };
	auto u16 = [&](size_t o) { return (unsigned)(b[o] | b[o + 1] << 8); };
	auto u32 = [&](size_t o) { return (unsigned)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24); };
	auto str = [&](size_t o, size_t n) { std::string s; for (size_t i = 0; i != n && b[o + i]; ++i) s += (char)b[o + i]; return s; };
	std::printf("frames %u seed %u (%#x) player bytes", u32(1), u32(8), u32(8));
	for (int i = 0; i != 8; ++i) std::printf(" %02x", u8(12 + i));
	std::printf("\nhost '%s' map %ux%u players %u slots %u speed %u state %u type %u/%u tileset %u\n", str(24, 24).c_str(),
		u16(52), u16(54), u8(56), u8(57), u8(58), u8(59), u16(60), u16(62), u16(68));
	std::printf("game '%s' map '%s'\n", str(72, 25).c_str(), str(97, 32).c_str());
	for (size_t o = 129; o != 161; ++o) std::printf("%02x ", u8(o));
	std::printf("\n");
	for (int i = 0; i != 12; ++i) {
		size_t o = 161 + 36 * i;
		std::printf("slot %2d: %08x id %u controller %u race %u force %u '%s'\n", i, u32(o), u32(o + 4), u8(o + 8), u8(o + 9), u8(o + 10), str(o + 11, 25).c_str());
	}
	std::printf("colours");
	for (int i = 0; i != 8; ++i) std::printf(" %u", u32(593 + 4 * i));
	std::printf("\nmelee units");
	for (int i = 0; i != 8; ++i) std::printf(" %u", u8(625 + i));
	std::printf("\n");
}

int info(const char* rep) {
	auto s = read_replay(rep);
	print_info(s.info);
	std::printf("actions %zu bytes, map %zu bytes\ngame info:", s.actions.size(), s.map.size());
	for (size_t i = 0; i != s.info.size(); ++i) std::printf("%s%02x", i % 32 ? " " : "\n", s.info[i]);
	std::printf("\n");
	return 0;
}

int compare(const char* rep, const char* live, const char* data_dir, size_t chunk) {
	auto load_data_file = data_loading::data_files_directory(data_dir);
	auto s = read_replay(rep);

	live_stream::reader lr;
	auto bytes = read_all(live);
	lr.feed(bytes.data(), bytes.size());
	if (lr.failed()) { std::printf("live replay: %s\n", lr.error().c_str()); return 2; }
	if (!lr.has_setup()) { std::printf("live replay: no setup yet\n"); return 2; }
	std::vector<uint8_t> live_actions = lr.take_actions();
	std::printf("live: known through frame %d%s, %zu action bytes; replay: %zu action bytes\n", lr.known_frame(),
		lr.ended() ? " (ended)" : "", live_actions.size(), s.actions.size());

	game_player ga(load_data_file), gb(load_data_file);  // each owns its own game state
	replay_player a(ga), b(gb);
	auto ra = raw_replay(s.info, s.actions, scenario_chk(s.map));
	a.load_replay_data(ra.data(), ra.size());

	// The live replay is loaded the way the viewer follows a game: its setup with what has
	// arrived so far, then more as it comes (live_replay::append / before_frame). With
	// `chunk` > 0 the file arrives in pieces of that many bytes, as from a decoder writing it.
	live_stream::reader inc;
	size_t fed = 0;
	auto feed_more = [&]() {
		size_t n = chunk ? std::min(chunk, bytes.size() - fed) : bytes.size() - fed;
		inc.feed(bytes.data() + fed, n);
		fed += n;
	};
	while (!inc.has_setup() && fed < bytes.size()) feed_more();
	{
		auto first = inc.take_actions();
		auto rb = live_replay::raw_replay(inc.game_info(), (uint32_t)std::max(1, inc.known_frame() + 1), first, scenario_chk(inc.map()));
		b.load_replay_data(rb.data(), rb.size());
	}
	int pieces = 0;

	int frames = 0, last_ok = -1;
	while (!a.is_done()) {
		while (b.is_done() && fed < bytes.size()) {  // at the newest frame: wait for more
			feed_more();
			live_replay::append(b.replay_st, inc.take_actions(), inc.known_frame());
			++pieces;
		}
		if (b.is_done()) break;
		if (state_hash(a.st()) != state_hash(b.st())) break;
		last_ok = a.st().current_frame;
		a.next_frame();
		live_replay::before_frame(b.replay_st, b.action_st);
		b.next_frame();
		++frames;
	}
	if (chunk) std::printf("fed in %d pieces of %zu bytes\n", pieces, chunk);
	bool same = state_hash(a.st()) == state_hash(b.st());
	if (same) last_ok = a.st().current_frame;
	// A replay never holds its own player's Leave Game: if that player quit, the live replay
	// (which has every player's commands) parts from it on that frame, and that is expected.
	bool own_leave = false;
	if (!same) {
		std::string saver;
		for (size_t i = 24; i != 48 && s.info[i]; ++i) saver += (char)s.info[i];
		for (size_t pos = 0; pos + 5 <= live_actions.size();) {
			int32_t frame;
			std::memcpy(&frame, live_actions.data() + pos, 4);
			size_t n = live_actions[pos + 4];
			// bwlive.py writes each Leave Game (0x57, reason) as a record of its own.
			{
				size_t k = pos + 5;
				if (n == 3 && live_actions[k + 1] == 0x57 && frame == last_ok) {
					int sid = live_actions[k];
					for (int slot = 0; slot != 12; ++slot) {
						if (a.action_st.player_id[slot] == sid && a.replay_st.player_name[slot].c_str() == saver) own_leave = true;
					}
				}
			}
			pos += 5 + n;
		}
	}
	std::printf("%s: states agree through frame %d (replay ends %d, live known %d)\n",
		same ? "OK" : own_leave ? "OK (then the replay's own player leaves)" : "DIVERGED", last_ok, a.replay_st.end_frame, lr.known_frame());
	for (int p = 0; p != 8; ++p) {
		int na = count_units(a.st(), p), nb = count_units(b.st(), p);
		if (na || nb) std::printf("  player %d: units %d / %d, minerals %d / %d\n", p, na, nb, a.st().current_minerals[p], b.st().current_minerals[p]);
	}
	return same || own_leave ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
	try {
		std::string cmd = argc > 1 ? argv[1] : "";
		if (cmd == "info" && argc == 3) return info(argv[2]);
		if (cmd == "compare" && (argc == 5 || argc == 6)) return compare(argv[2], argv[3], argv[4], argc == 6 ? (size_t)std::stoul(argv[5]) : 0);
		std::fprintf(stderr, "usage: live_check info <replay.rep> | compare <replay.rep> <game.bwlv> <data-dir> [chunk-bytes]\n");
		return 2;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
}
