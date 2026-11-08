#ifndef OPENBW_LIVE_REPLAY_H
#define OPENBW_LIVE_REPLAY_H

// A live replay's setup (ui/live_stream.h) as an uncompressed replay that OpenBW's replay
// loader reads (replay_functions::load_replay with a plain data_reader_le): the game info with
// its frame count set, the action records so far, and the map's scenario.chk.

#include "../bwgame.h"
#include "../replay.h"
#include "live_stream.h"

#include <cstring>
#include <vector>

namespace bwgame {
namespace live_replay {

// The map's scenario.chk: as is, or out of the .scx/.scm (MPQ) archive holding it.
inline std::vector<uint8_t> scenario_chk(std::vector<uint8_t> map) {
	if (map.size() < 4 || std::memcmp(map.data(), "MPQ\x1a", 4)) return map;
	data_loading::mpq_data mpq(map.data(), map.size());
	a_vector<uint8_t> chk;
	mpq(chk, "staredit\\scenario.chk");
	return std::vector<uint8_t>(chk.begin(), chk.end());
}

inline std::vector<uint8_t> raw_replay(std::array<uint8_t, live_stream::game_info_size> info, uint32_t frame_count,
	const std::vector<uint8_t>& actions, const std::vector<uint8_t>& chk) {
	for (int i = 0; i != 4; ++i) info[1 + i] = (uint8_t)(frame_count >> (8 * i));
	std::vector<uint8_t> out;
	auto u32 = [&](uint32_t v) { for (int i = 0; i != 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
	u32(0x53526572);
	out.insert(out.end(), info.begin(), info.end());
	u32((uint32_t)actions.size());
	out.insert(out.end(), actions.begin(), actions.end());
	u32((uint32_t)chk.size());
	out.insert(out.end(), chk.begin(), chk.end());
	return out;
}

// Add newly arrived action records to a loaded replay, and move its end to just past the
// newest known frame (frames up to it can be simulated).
inline void append(replay_state& replay_st, const std::vector<uint8_t>& actions, int known_frame) {
	auto& buf = replay_st.actions_data_buffer;
	buf.insert(buf.end(), actions.begin(), actions.end());
	if (known_frame + 1 > replay_st.end_frame) replay_st.end_frame = known_frame + 1;
}

// Call before simulating each frame of a replay that grows. The actions reader stops looking
// once it has read everything there was (it then waits for the frame number it saw last), so
// point it at the next record, which may have arrived since: what it would have found itself.
inline void before_frame(const replay_state& replay_st, action_state& action_st) {
	auto& buf = replay_st.actions_data_buffer;
	size_t pos = action_st.actions_data_position;
	if (pos + 4 <= buf.size()) {
		int32_t frame;
		std::memcpy(&frame, buf.data() + pos, 4);
		action_st.next_action_frame = frame;
	}
}

}  // namespace live_replay
}  // namespace bwgame

#endif
