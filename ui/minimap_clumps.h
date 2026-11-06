#ifndef OPENBW_UI_MINIMAP_CLUMPS_H
#define OPENBW_UI_MINIMAP_CLUMPS_H

// Army clumps on the minimap: groups of one player's army units, each labelled with small
// icons of the unit types that make it up by cost, so a stack of Mutalisks drawn as one dot
// still reads as Mutalisks, and Zerglings with Mutalisks as both. Pure: no game state, so it can be tested on its own
// (viewer/tests/minimap_clumps_test.cpp).
//
// Grouping is single-link (DBSCAN with one point per core: units within `link` of one
// another share a group), which follows an army of any shape. Single linkage chains, though:
// a column of units strung along a path links into one group whose centre is in empty
// ground. A group spread wider than `split_rms` is cut in two by 2-means, repeatedly (up to
// `max_depth` times), so its labels land on the actual concentrations.
//
// Badges are placed greedily in the order given (the caller passes the biggest clumps
// first), on the clump's centre and inside the minimap; a badge that would overlap one
// already placed is left out. As in map marker clustering, labels that collide at the
// display's scale merge into the most important one rather than drifting off their units.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace minimap_clumps {

struct point {
	int x = 0, y = 0;     // map pixels
	int type = -1;        // unit type, normalized by the caller (e.g. siege mode as tank mode)
	int supply_half = 0;  // as the game counts it: a Zergling is 1, a Mutalisk 4
	int value = 0;        // minerals + gas
};

struct clump {
	int x = 0, y = 0;  // centre, map pixels
	int radius = 0;    // map pixels
	int supply_half = 0;
	int units = 0;
	// The types worth naming, most value first (at least one): the top type, and each other
	// with a share of the value or a supply of its own (see params), up to max_types.
	std::vector<int> types;
};

struct params {
	int link = 6 * 32;          // units this close share a group
	int split_rms = 6 * 32;     // a group spread wider than this is split
	int max_depth = 3;          // at most 2^3 parts from one group
	int max_radius = 8 * 32;
	int min_supply_half = 16;   // 8 supply: four Mutalisks or eight Marines, not two Zerglings
	int max_types = 3;
	double type_share = 0.25;       // a further type is named with this share of the value,
	int type_supply_half = 12;      // or 6 supply of its own: 20 Zerglings beside 8 Mutalisks
};

namespace detail {
inline double rms(const std::vector<point>& pts, const std::vector<size_t>& idx, double& cx, double& cy) {
	cx = cy = 0;
	for (size_t i : idx) cx += pts[i].x, cy += pts[i].y;
	cx /= idx.size(), cy /= idx.size();
	double sq = 0;
	for (size_t i : idx) sq += (pts[i].x - cx) * (pts[i].x - cx) + (pts[i].y - cy) * (pts[i].y - cy);
	return std::sqrt(sq / idx.size());
}
inline double d2(const point& a, double x, double y) { return (a.x - x) * (a.x - x) + (a.y - y) * (a.y - y); }

// Cut a spread group in two by 2-means, seeded with its two extremes; recurse on the parts.
inline void split(const std::vector<point>& pts, std::vector<size_t> idx, int depth, const params& p, std::vector<std::vector<size_t>>& out) {
	double cx, cy;
	if (idx.size() < 2 || depth >= p.max_depth || rms(pts, idx, cx, cy) <= p.split_rms) {
		out.push_back(std::move(idx));
		return;
	}
	auto farthest = [&](double x, double y) {
		size_t best = idx[0];
		for (size_t i : idx) if (d2(pts[i], x, y) > d2(pts[best], x, y)) best = i;
		return best;
	};
	size_t a = farthest(cx, cy), b = farthest(pts[a].x, pts[a].y);
	double ax = pts[a].x, ay = pts[a].y, bx = pts[b].x, by = pts[b].y;
	std::vector<size_t> l, r;
	for (int iter = 0; iter != 8; ++iter) {
		l.clear(), r.clear();
		for (size_t i : idx) (d2(pts[i], ax, ay) <= d2(pts[i], bx, by) ? l : r).push_back(i);
		if (l.empty() || r.empty()) break;
		double nax, nay, nbx, nby;
		rms(pts, l, nax, nay), rms(pts, r, nbx, nby);
		if (nax == ax && nay == ay && nbx == bx && nby == by) break;
		ax = nax, ay = nay, bx = nbx, by = nby;
	}
	if (l.empty() || r.empty()) {
		out.push_back(std::move(idx));
		return;
	}
	split(pts, std::move(l), depth + 1, p, out);
	split(pts, std::move(r), depth + 1, p, out);
}
}

// The clumps among one player's army units, biggest (by supply) first.
inline std::vector<clump> find(const std::vector<point>& pts, const params& p = {}) {
	const size_t n = pts.size();
	std::vector<int> group(n, -1);
	std::vector<std::vector<size_t>> groups;
	const double link2 = (double)p.link * p.link;
	for (size_t s = 0; s != n; ++s) {
		if (group[s] >= 0) continue;
		std::vector<size_t> members{s}, todo{s};
		group[s] = (int)groups.size();
		while (!todo.empty()) {
			size_t a = todo.back();
			todo.pop_back();
			for (size_t b = 0; b != n; ++b) {
				if (group[b] >= 0 || detail::d2(pts[a], pts[b].x, pts[b].y) > link2) continue;
				group[b] = group[s];
				todo.push_back(b);
				members.push_back(b);
			}
		}
		groups.push_back(std::move(members));
	}
	std::vector<std::vector<size_t>> parts;
	for (auto& g : groups) detail::split(pts, std::move(g), 0, p, parts);

	std::vector<clump> out;
	for (auto& idx : parts) {
		clump c;
		for (size_t i : idx) c.supply_half += std::max(0, pts[i].supply_half);
		if (c.supply_half < p.min_supply_half) continue;
		double cx, cy;
		double r = detail::rms(pts, idx, cx, cy);
		c.x = (int)std::lround(cx), c.y = (int)std::lround(cy);
		// 1.2 x the RMS distance: about the group's extent, without stragglers blowing it up.
		c.radius = std::min(p.max_radius, (int)(1.2 * r));
		c.units = (int)idx.size();
		// Value per type (supply breaks ties, then the lower type, so the label is stable).
		struct tally { int type; int64_t value; int supply; };
		std::vector<tally> t;
		int64_t total = 0;
		for (size_t i : idx) {
			int64_t v = std::max(0, pts[i].value);
			total += v;
			auto it = std::find_if(t.begin(), t.end(), [&](const tally& x) { return x.type == pts[i].type; });
			if (it == t.end()) t.push_back({pts[i].type, v, pts[i].supply_half});
			else it->value += v, it->supply += pts[i].supply_half;
		}
		std::sort(t.begin(), t.end(), [](const tally& a, const tally& b) {
			if (a.value != b.value) return a.value > b.value;
			if (a.supply != b.supply) return a.supply > b.supply;
			return a.type < b.type;
		});
		for (size_t k = 0; k != t.size() && (int)c.types.size() < std::max(1, p.max_types); ++k) {
			bool named = k == 0 || (total > 0 && t[k].value >= p.type_share * total) || t[k].supply >= p.type_supply_half;
			if (named) c.types.push_back(t[k].type);
		}
		out.push_back(c);
	}
	std::stable_sort(out.begin(), out.end(), [](const clump& a, const clump& b) { return a.supply_half > b.supply_half; });
	return out;
}

// Badge placement on a w x h minimap. `want` holds each badge's centre and size, in order of
// importance; the result says where each top-left corner goes, or placed = false.
struct badge { int cx = 0, cy = 0, w = 0, h = 0; };
struct placement { int x = 0, y = 0; bool placed = false; };
inline std::vector<placement> place(const std::vector<badge>& want, int w, int h, int pad = 1) {
	std::vector<placement> out(want.size());
	struct box { int x0, y0, x1, y1; };
	std::vector<box> taken;
	for (size_t i = 0; i != want.size(); ++i) {
		const badge& b = want[i];
		if (b.w <= 0 || b.h <= 0 || b.w > w || b.h > h) continue;
		int x = std::max(0, std::min(w - b.w, b.cx - b.w / 2));
		int y = std::max(0, std::min(h - b.h, b.cy - b.h / 2));
		box c{x, y, x + b.w, y + b.h};
		bool clear = std::none_of(taken.begin(), taken.end(), [&](const box& t) {
			return c.x0 < t.x1 + pad && t.x0 < c.x1 + pad && c.y0 < t.y1 + pad && t.y0 < c.y1 + pad;
		});
		if (!clear) continue;
		taken.push_back(c);
		out[i] = {x, y, true};
	}
	return out;
}

}

#endif
