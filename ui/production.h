#ifndef OPENBW_PRODUCTION_H
#define OPENBW_PRODUCTION_H

// What each player has in production (units, buildings, upgrades and research) and what
// finished lately. The viewer lists the items in progress every simulated frame (each with
// what it is, who makes it and how long it has left); this keeps the lists, groups them for
// display, and notices completions: an item seen with a frame or two left that is gone on the
// next frame (or replaced by the next in a queue, with its full time left) finished then.
// One that disappears earlier was cancelled or destroyed, not
// finished. Seeking back and simulating again sees the same completions again; they are kept
// once. A jump (a seek) is not read as completions. Pure, so it can be tested.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace production {

enum class kind { unit, building, upgrade, research };

struct item {
	int player = 0;
	kind what = kind::unit;
	int type = 0;        // unit, upgrade or tech type id
	int level = 0;       // upgrades: the level being researched
	uint32_t source = 0; // what makes it (a building, an egg): with what/type/level, identifies it
	int remaining = 0;   // frames left
	int total = 1;       // frames it takes in all
	int priority = 0;    // display order among units and buildings: higher first (the viewer's
	                     // choice: tech and expensive things ahead, workers last)
};

// Items of one kind shown as one row: "Zealot x2", soonest done first.
struct group {
	kind what = kind::unit;
	int type = 0, level = 0;
	int count = 0;
	int remaining = 0;  // the soonest
	double progress = 0;  // of the soonest, 0..1
	int priority = 0;
};

struct finished {
	int frame = 0;
	int player = 0;
	kind what = kind::unit;
	int type = 0, level = 0;
	int priority = 0;
};

// Display order of the kinds: research first, then upgrades, then units and buildings.
inline int kind_rank(kind k) {
	return k == kind::research ? 0 : k == kind::upgrade ? 1 : 2;
}
// Whether `a` is listed before `b` by kind, then priority (higher first); ties are left to the
// caller (time).
template <typename T>
inline int compare_rank(const T& a, const T& b) {
	int ra = kind_rank(a.what), rb = kind_rank(b.what);
	if (ra != rb) return ra < rb ? -1 : 1;
	if (a.priority != b.priority) return a.priority > b.priority ? -1 : 1;
	return 0;
}

// In-progress items grouped by (kind, type, level) for one player: research, then upgrades,
// then the rest by priority (compare_rank), each soonest finishing first.
inline std::vector<group> grouped(const std::vector<item>& items, int player) {
	std::map<std::tuple<int, int, int>, group> g;
	for (const item& it : items) {
		if (it.player != player) continue;
		auto key = std::make_tuple((int)it.what, it.type, it.level);
		group& e = g[key];
		int rem = std::max(0, it.remaining);
		double prog = it.total > 0 ? std::max(0.0, std::min(1.0, 1.0 - (double)rem / it.total)) : 0;
		if (e.count == 0 || rem < e.remaining) e.remaining = rem, e.progress = prog;
		e.what = it.what, e.type = it.type, e.level = it.level, e.priority = it.priority;
		++e.count;
	}
	std::vector<group> out;
	for (auto& v : g) out.push_back(v.second);
	std::stable_sort(out.begin(), out.end(), [](const group& a, const group& b) {
		int c = compare_rank(a, b);
		return c ? c < 0 : a.remaining < b.remaining;
	});
	return out;
}

// Completions grouped by (kind, type, level): "Probe x5", the newest frame of each; research,
// then upgrades, then the rest by priority (compare_rank), each newest first.
struct finished_group {
	kind what = kind::unit;
	int type = 0, level = 0;
	int count = 0;
	int newest = 0;  // frame of the latest
	int priority = 0;
};
inline std::vector<finished_group> grouped(const std::vector<finished>& done) {
	std::map<std::tuple<int, int, int>, finished_group> g;
	for (const finished& f : done) {
		finished_group& e = g[std::make_tuple((int)f.what, f.type, f.level)];
		if (e.count == 0 || f.frame > e.newest) e.newest = f.frame;
		e.what = f.what, e.type = f.type, e.level = f.level, e.priority = f.priority;
		++e.count;
	}
	std::vector<finished_group> out;
	for (auto& v : g) out.push_back(v.second);
	std::stable_sort(out.begin(), out.end(), [](const finished_group& a, const finished_group& b) {
		int c = compare_rank(a, b);
		return c ? c < 0 : a.newest > b.newest;
	});
	return out;
}

class tracker {
public:
	// The items in production at `frame` (call once per simulated frame).
	void update(int frame, std::vector<item> items) {
		if (frame == last_frame + 1) {
			for (const item& was : current) {
				if (was.remaining > done_within) continue;
				// Still the same item if it is there counting down; one there with more time left is
				// the next in a queue, begun as this one finished.
				bool still = std::any_of(items.begin(), items.end(), [&](const item& now) { return same(was, now) && now.remaining <= was.remaining; });
				if (!still) add({frame, was.player, was.what, was.type, was.level, was.priority}, was.source);
			}
		}
		current = std::move(items);
		last_frame = frame;
	}
	const std::vector<item>& now() const { return current; }

	// `player`'s completions in the `window` frames up to and including `frame`, newest first.
	std::vector<finished> recent(int player, int frame, int window) const {
		std::vector<finished> out;
		for (auto& v : done) {
			const finished& f = v.second;
			if (f.player == player && f.frame <= frame && f.frame > frame - window) out.push_back(f);
		}
		std::stable_sort(out.begin(), out.end(), [](const finished& a, const finished& b) { return a.frame > b.frame; });
		return out;
	}

	void clear() {
		current.clear();
		done.clear();
		last_frame = -2;
	}

private:
	static constexpr int done_within = 2;  // frames left when last seen, for a disappearance to be a completion
	std::vector<item> current;
	int last_frame = -2;
	// Completions, once each: by where they came from, what they were and when.
	std::map<std::tuple<uint32_t, int, int, int, int>, finished> done;

	static bool same(const item& a, const item& b) {
		return a.source == b.source && a.what == b.what && a.type == b.type && a.level == b.level && a.player == b.player;
	}
	void add(const finished& f, uint32_t source) {
		done.emplace(std::make_tuple(source, (int)f.what, f.type, f.level, f.frame), f);
	}
};

}  // namespace production

#endif
