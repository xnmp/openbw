#ifndef OPENBW_MINIMAP_MERGE_H
#define OPENBW_MINIMAP_MERGE_H

// Minimap dots of one kind (one player's units of one type) that sit almost on top of each
// other combine into one larger dot, so a ball of 12 Mutalisks reads as one heavy mark rather
// than a smear. Only very close dots merge: their centres must be within `link` of the sum of
// their radii (with the default, the two overlap by about 70%). Merged dots keep merging by the
// same rule until nothing changes. A merged dot sits at its members' centroid, its size set by
// their total cost on one scale for every unit type (a dot of radius `unit_r` stands for
// `ref_cost`): twelve Mutalisks outweigh twelve Zerglings. Its radius grows with the cube root
// of the cost (`growth`), as if the dot were a ball holding the army, so a stack stays readable
// next to single units: nine Wraiths are about twice one Wraith across, not three times as
// area-true sizing would make them. It is never smaller than one of its members and at most
// `max_scale` times `unit_r`. Groups smaller than `min_count` stay separate dots. A column of
// units does not chain into one dot: each merge needs the dots to overlap well, and a merged
// dot grows only by the cube root of its cost. Pure maths, so it can be tested.

#include <algorithm>
#include <cmath>
#include <vector>

namespace minimap_merge {

struct dot {
	double x = 0, y = 0, r = 0;  // centre and radius, minimap pixels
	int key = 0;                 // dots merge only with dots of the same key
	double cost = 0;             // what the unit is worth (minerals + gas)
};

struct blob {
	double x = 0, y = 0, r = 0;
	int key = 0;
	int n = 1;      // dots it stands for
	int first = 0;  // its first dot's index in the input (for colour and draw order)
	double r1 = 0;  // its dots' typical radius (r for a single dot)
};

struct params {
	double link = 0.3;
	int min_count = 3;
	double unit_r = 0, ref_cost = 150;  // the cost scale (unit_r <= 0: area grows with the count)
	double max_scale = 4;               // of unit_r (or of the members' own radius without a scale)
	double growth = 1.0 / 3;            // radius ~ cost^growth (or count^growth without a scale)
};

// The dots to draw: merged groups and the dots left alone, in the order of their first dot.
// Dots with a position that is not a number are dropped; a radius that is not positive is 0.
inline std::vector<blob> merge(const std::vector<dot>& dots, const params& p = {}) {
	struct group {
		double sx = 0, sy = 0, area = 0, cost = 0;  // sums over members
		int key = 0;
		std::vector<int> members;
		bool alive = true;
		int n() const { return (int)members.size(); }
		double x() const { return sx / n(); }
		double y() const { return sy / n(); }
	};
	const double cap = std::max(1.0, std::isfinite(p.max_scale) ? p.max_scale : 1.0);
	const bool by_cost = std::isfinite(p.unit_r) && p.unit_r > 0 && std::isfinite(p.ref_cost) && p.ref_cost > 0;
	const double growth = std::isfinite(p.growth) && p.growth > 0 && p.growth <= 1 ? p.growth : 1.0 / 3;
	auto radius = [&](const group& a) {
		double member = std::sqrt(a.area / a.n());
		if (a.n() == 1) return member;
		if (!by_cost) return member * std::min(std::pow((double)a.n(), growth), cap);
		double r = std::min(p.unit_r * std::pow(a.cost / p.ref_cost, growth), p.unit_r * cap);
		return std::max(member, r);
	};
	std::vector<group> g;
	g.reserve(dots.size());
	for (size_t i = 0; i != dots.size(); ++i) {
		const dot& d = dots[i];
		if (!std::isfinite(d.x) || !std::isfinite(d.y)) continue;
		double r = std::isfinite(d.r) && d.r > 0 ? d.r : 0;
		group a;
		a.sx = d.x, a.sy = d.y, a.area = r * r, a.key = d.key, a.members = {(int)i};
		a.cost = std::isfinite(d.cost) && d.cost > 0 ? d.cost : 0;
		g.push_back(std::move(a));
	}
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t i = 0; i != g.size(); ++i) {
			if (!g[i].alive) continue;
			for (size_t j = i + 1; j != g.size(); ++j) {
				group& a = g[i];
				group& b = g[j];
				if (!b.alive || b.key != a.key) continue;
				double dx = a.x() - b.x(), dy = a.y() - b.y(), reach = p.link * (radius(a) + radius(b));
				if (dx * dx + dy * dy > reach * reach) continue;
				a.sx += b.sx, a.sy += b.sy, a.area += b.area, a.cost += b.cost;
				a.members.insert(a.members.end(), b.members.begin(), b.members.end());
				b.alive = false;
				changed = true;
			}
		}
	}
	std::vector<blob> out;
	for (const group& a : g) {
		if (!a.alive) continue;
		if (a.n() >= std::max(2, p.min_count)) {
			out.push_back({a.x(), a.y(), radius(a), a.key, a.n(), *std::min_element(a.members.begin(), a.members.end()), std::sqrt(a.area / a.n())});
		} else {
			for (int i : a.members) {
				const dot& d = dots[i];
				double r = std::isfinite(d.r) && d.r > 0 ? d.r : 0;
				out.push_back({d.x, d.y, r, d.key, 1, i, r});
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const blob& a, const blob& b) { return a.first < b.first; });
	return out;
}

}  // namespace minimap_merge

#endif
