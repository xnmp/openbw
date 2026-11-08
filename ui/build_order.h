#ifndef OPENBW_BUILD_ORDER_H
#define OPENBW_BUILD_ORDER_H

// Each player's buildings in the order they were started, with the supply count and game time
// at the start, as build orders are written ("9 Pool, 1:20"). The viewer records a building
// when it sees it begin while simulating; seeking back and simulating again sees the same
// starts again, so a start is kept once per (unit, building type) and the list shown is the
// starts up to the frame on screen. Pure, so it can be tested.

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace build_order {

struct entry {
	int frame = 0;
	int player = 0;
	int type = 0;    // unit type id of the building
	int supply = 0;  // supply count (whole units) when it was started
};

class log {
public:
	// A building of `type`, unit `uid` (an id that is not reused, e.g. index and generation),
	// started on `frame`. The first report of a (unit, type) counts; repeats are ignored.
	void add(uint32_t uid, int frame, int player, int type, int supply) {
		if (frame < 0 || player < 0) return;
		starts.emplace(std::make_pair(uid, type), entry{frame, player, type, std::max(0, supply)});
	}
	bool has(uint32_t uid, int type) const { return starts.count(std::make_pair(uid, type)) != 0; }

	// `player`'s starts up to and including `frame`, in order (ties keep the unit order).
	std::vector<entry> upto(int player, int frame) const {
		std::vector<std::pair<uint32_t, entry>> r;
		for (auto& v : starts) {
			if (v.second.player == player && v.second.frame <= frame) r.push_back({v.first.first, v.second});
		}
		std::sort(r.begin(), r.end(), [](const std::pair<uint32_t, entry>& a, const std::pair<uint32_t, entry>& b) {
			return a.second.frame != b.second.frame ? a.second.frame < b.second.frame : a.first < b.first;
		});
		std::vector<entry> out;
		for (auto& v : r) out.push_back(v.second);
		return out;
	}

	void clear() { starts.clear(); }

private:
	std::map<std::pair<uint32_t, int>, entry> starts;
};

}  // namespace build_order

#endif
