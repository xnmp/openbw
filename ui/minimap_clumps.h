#ifndef OPENBW_UI_MINIMAP_CLUMPS_H
#define OPENBW_UI_MINIMAP_CLUMPS_H

// Army icons on the minimap: where one player has a concentration of one unit type, an icon
// of that type, so a stack of Mutalisks drawn as one dot still reads as Mutalisks. Pure: no
// game state, so it can be tested on its own (viewer/tests/minimap_clumps_test.cpp).
//
// Each unit type is grouped on its own. Grouping is single-link (DBSCAN with one point per
// core: units within `link` of one another share a group), which follows an army of any
// shape. Single linkage chains, though: a column strung along a path links into one group
// whose centre is in empty ground. So groups have a maximum diameter: one whose farthest
// two units are more than `max_diameter` apart is cut in two by 2-means, repeatedly
// (bisecting k-means), until every part fits.
//
// A group earns an icon only if both its total cost (minerals + gas) and its density (cost
// per square tile over the area its units spread across) reach a threshold: a real force,
// and concentrated, not a few units or a thin scatter. Icons of one player that would land
// on top of each other (Zerglings under their Mutalisks) share one row, most cost first.
//
// Rows are then placed greedily, most cost first, on their centre and inside the minimap; a
// row that would overlap one already placed is left out. As in map marker clustering,
// labels that collide at the display's scale merge into the most important one rather than
// drifting off their units.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace minimap_clumps {

struct point {
	int x = 0, y = 0;  // map pixels
	int type = -1;     // unit type, normalized by the caller (e.g. siege mode as tank mode)
	int value = 0;     // minerals + gas
};

// One unit type's concentration that earns an icon.
struct group {
	int type = -1;
	int x = 0, y = 0;    // centre, map pixels
	int radius = 0;      // RMS distance of its units from the centre, map pixels
	int diameter = 0;    // farthest two units, map pixels
	int64_t cost = 0;
	int units = 0;
	double density = 0;  // cost per square tile
};

// A row of icons on the map: one or more groups of one player at about the same place.
struct label {
	int x = 0, y = 0;  // centre, map pixels (cost-weighted)
	int64_t cost = 0;
	std::vector<int> types;  // most cost first
};

struct params {
	int link = 6 * 32;            // units this close share a group
	int max_diameter = 10 * 32;   // no two units of a group farther apart than this
	int64_t min_cost = 750;       // 30 Zerglings, 4 Mutalisks, 15 Marines, 5 Dragoons
	double min_density = 12;      // cost per square tile
	int min_radius = 2 * 32;      // a stack counts as spread over at least this radius
	int merge_distance = 4 * 32;  // groups' centres this close share a row
	int max_types = 3;            // icons in one row
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

// Cut a group in two by 2-means, seeded with its two extremes; false if it won't divide.
inline bool two_means(const std::vector<point>& pts, const std::vector<size_t>& idx, std::vector<size_t>& l, std::vector<size_t>& r) {
	double cx, cy;
	rms(pts, idx, cx, cy);
	auto farthest = [&](double x, double y) {
		size_t best = idx[0];
		for (size_t i : idx) if (d2(pts[i], x, y) > d2(pts[best], x, y)) best = i;
		return best;
	};
	size_t a = farthest(cx, cy), b = farthest(pts[a].x, pts[a].y);
	double ax = pts[a].x, ay = pts[a].y, bx = pts[b].x, by = pts[b].y;
	for (int iter = 0; iter != 8; ++iter) {
		l.clear(), r.clear();
		for (size_t i : idx) (d2(pts[i], ax, ay) <= d2(pts[i], bx, by) ? l : r).push_back(i);
		if (l.empty() || r.empty()) return false;
		double nax, nay, nbx, nby;
		rms(pts, l, nax, nay), rms(pts, r, nbx, nby);
		if (nax == ax && nay == ay && nbx == bx && nby == by) break;
		ax = nax, ay = nay, bx = nbx, by = nby;
	}
	return true;
}

// The group of these units if it earns an icon (cost and density), else cost 0.
inline group judge(const std::vector<point>& pts, const std::vector<size_t>& idx, int type, const params& p) {
	group g;
	g.type = type;
	for (size_t i : idx) g.cost += std::max(0, pts[i].value);
	double cx, cy;
	double r = rms(pts, idx, cx, cy);
	double rt = std::max<double>(r, p.min_radius) / 32;
	g.density = g.cost / (3.14159265358979 * rt * rt);
	g.x = (int)std::lround(cx), g.y = (int)std::lround(cy);
	g.radius = (int)r;
	g.units = (int)idx.size();
	if (g.cost < p.min_cost || g.density < p.min_density) g.cost = 0;
	return g;
}

inline double diameter(const std::vector<point>& pts, const std::vector<size_t>& idx) {
	double best = 0;
	for (size_t i = 0; i != idx.size(); ++i) {
		for (size_t j = i + 1; j != idx.size(); ++j) best = std::max(best, d2(pts[idx[i]], pts[idx[j]].x, pts[idx[j]].y));
	}
	return std::sqrt(best);
}

// The parts of a group no wider than max_diameter (bisecting it as needed) that earn icons.
inline void concentrations(const std::vector<point>& pts, const std::vector<size_t>& idx, int type, int depth, const params& p, std::vector<group>& out) {
	double d = diameter(pts, idx);
	if (d > p.max_diameter) {
		// Each cut leaves both parts non-empty, so this ends; the depth bound is a guard
		// against a pathological input, and an oversized part is never shown.
		std::vector<size_t> l, r;
		if (depth >= 32 || !two_means(pts, idx, l, r)) return;
		concentrations(pts, l, type, depth + 1, p, out);
		concentrations(pts, r, type, depth + 1, p, out);
		return;
	}
	group g = judge(pts, idx, type, p);
	g.diameter = (int)d;
	if (g.cost > 0) out.push_back(g);
}
}

// One player's unit types' groups that earn an icon, most cost first (then lower type).
inline std::vector<group> find_groups(const std::vector<point>& pts, const params& p = {}) {
	std::vector<int> types;
	for (auto& q : pts) if (std::find(types.begin(), types.end(), q.type) == types.end()) types.push_back(q.type);
	std::vector<group> out;
	const double link2 = (double)p.link * p.link;
	for (int type : types) {
		std::vector<size_t> of_type;
		for (size_t i = 0; i != pts.size(); ++i) if (pts[i].type == type) of_type.push_back(i);
		// Single-link groups among this type's units, then their concentrations.
		std::vector<char> seen(pts.size(), 0);
		for (size_t s0 : of_type) {
			if (seen[s0]) continue;
			std::vector<size_t> members{s0}, todo{s0};
			seen[s0] = 1;
			while (!todo.empty()) {
				size_t a = todo.back();
				todo.pop_back();
				for (size_t b : of_type) {
					if (seen[b] || detail::d2(pts[a], pts[b].x, pts[b].y) > link2) continue;
					seen[b] = 1;
					todo.push_back(b);
					members.push_back(b);
				}
			}
			detail::concentrations(pts, members, type, 0, p, out);
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const group& a, const group& b) {
		return a.cost != b.cost ? a.cost > b.cost : a.type < b.type;
	});
	return out;
}

// Rows of icons from one player's groups: each group, most cost first, joins the first row
// whose centre is within merge_distance and has room (or already shows its type: a big
// force split in parts stays one icon), else starts its own.
inline std::vector<label> rows(const std::vector<group>& groups, const params& p = {}) {
	std::vector<label> out;
	const double m2 = (double)p.merge_distance * p.merge_distance;
	for (auto& g : groups) {
		auto it = std::find_if(out.begin(), out.end(), [&](const label& l) {
			double dx = l.x - g.x, dy = l.y - g.y;
			bool has = std::find(l.types.begin(), l.types.end(), g.type) != l.types.end();
			return (has || (int)l.types.size() < p.max_types) && dx * dx + dy * dy <= m2;
		});
		if (it == out.end()) {
			out.push_back({g.x, g.y, g.cost, {g.type}});
			continue;
		}
		int64_t c = it->cost + g.cost;
		if (c > 0) {
			it->x = (int)((it->x * it->cost + (int64_t)g.x * g.cost) / c);
			it->y = (int)((it->y * it->cost + (int64_t)g.y * g.cost) / c);
		}
		it->cost = c;
		if (std::find(it->types.begin(), it->types.end(), g.type) == it->types.end()) it->types.push_back(g.type);
	}
	std::stable_sort(out.begin(), out.end(), [](const label& a, const label& b) { return a.cost > b.cost; });
	return out;
}

inline std::vector<label> find(const std::vector<point>& pts, const params& p = {}) {
	return rows(find_groups(pts, p), p);
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
