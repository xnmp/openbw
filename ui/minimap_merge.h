#ifndef OPENBW_MINIMAP_MERGE_H
#define OPENBW_MINIMAP_MERGE_H

// Minimap dots of one kind (one player's units of one type) that sit almost on top of each
// other combine into one larger dot, so a ball of 12 Mutalisks reads as one heavy mark rather
// than a smear. Only very close dots merge: their centres must be within `link` of the sum of
// their radii (with the default, the two overlap by about 70%). Merged dots keep merging by the
// same rule until nothing changes. A merged dot sits at its members' centroid with their total
// area (radius r * sqrt(n), capped at `max_scale` times a single dot), the usual sizing for a
// symbol that stands for a count. Groups smaller than `min_count` stay separate dots. A column
// of units does not chain into one dot: each merge needs the dots to overlap well, and a
// merged dot grows only by the square root of its count. Pure maths, so it can be tested.

#include <algorithm>
#include <cmath>
#include <vector>

namespace minimap_merge {

struct dot {
	double x = 0, y = 0, r = 0;  // centre and radius, minimap pixels
	int key = 0;                 // dots merge only with dots of the same key
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
	double max_scale = 4;
};

// The dots to draw: merged groups and the dots left alone, in the order of their first dot.
// Dots with a position that is not a number are dropped; a radius that is not positive is 0.
inline std::vector<blob> merge(const std::vector<dot>& dots, const params& p = {}) {
	struct group {
		double sx = 0, sy = 0, area = 0;  // sums over members
		int key = 0;
		std::vector<int> members;
		bool alive = true;
		int n() const { return (int)members.size(); }
		double x() const { return sx / n(); }
		double y() const { return sy / n(); }
	};
	const double cap = std::max(1.0, std::isfinite(p.max_scale) ? p.max_scale : 1.0);
	auto radius = [&](const group& a) { return std::sqrt(a.area / a.n()) * std::min(std::sqrt((double)a.n()), cap); };
	std::vector<group> g;
	g.reserve(dots.size());
	for (size_t i = 0; i != dots.size(); ++i) {
		const dot& d = dots[i];
		if (!std::isfinite(d.x) || !std::isfinite(d.y)) continue;
		double r = std::isfinite(d.r) && d.r > 0 ? d.r : 0;
		group a;
		a.sx = d.x, a.sy = d.y, a.area = r * r, a.key = d.key, a.members = {(int)i};
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
				a.sx += b.sx, a.sy += b.sy, a.area += b.area;
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
