// Behaviour tests for the minimap army clumps (ui/minimap_clumps.h).
// Build and run: cmake --build build --target openbw-minimap-clumps-test && ./build/openbw-minimap-clumps-test
#include "minimap_clumps.h"

#include <cstdio>

using minimap_clumps::point;
using minimap_clumps::badge;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

enum { zergling = 37, mutalisk = 43, hydralisk = 38, marine = 0, medic = 34, tank = 5 };

// `n` units of a type in a row from (x, y), `step` pixels apart.
static void add(std::vector<point>& v, int n, int type, int supply_half, int value, int x, int y, int step = 8) {
	for (int i = 0; i != n; ++i) v.push_back({x + i * step, y, type, supply_half, value});
}

int main() {
	{  // nothing, or too little army: no clumps
		CHECK(minimap_clumps::find({}).empty());
		std::vector<point> v;
		add(v, 3, mutalisk, 4, 200, 1000, 1000);  // 6 supply
		CHECK(minimap_clumps::find(v).empty());
	}
	{  // a stack of Mutalisks on one spot is a Mutalisk clump
		std::vector<point> v;
		add(v, 11, mutalisk, 4, 200, 1000, 1000, 0);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1);
		if (c.size() == 1) {
			CHECK(c[0].types == std::vector<int>{mutalisk});
			CHECK(c[0].units == 11);
			CHECK(c[0].x == 1000 && c[0].y == 1000);
			CHECK(c[0].radius == 0);
		}
	}
	{  // the order goes by cost, not head count: 6 Zerglings beside 4 Mutalisks and 3
	   // Hydralisks lead with the Mutalisk; the Zerglings (3 supply, 150 of 1325) aren't named
		std::vector<point> v;
		add(v, 6, zergling, 1, 25, 1000, 1000);
		add(v, 4, mutalisk, 4, 200, 1000, 1010);
		add(v, 3, hydralisk, 2, 125, 1000, 1020);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1);
		if (c.size() == 1) CHECK((c[0].types == std::vector<int>{mutalisk, hydralisk}));
	}
	{  // 20 Zerglings with 8 Mutalisks show both, though the Zerglings are only 500 of 2100:
	   // 10 supply is an army of its own
		std::vector<point> v;
		add(v, 20, zergling, 1, 25, 1000, 1000);
		add(v, 8, mutalisk, 4, 200, 1000, 1010);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1);
		if (c.size() == 1) CHECK((c[0].types == std::vector<int>{mutalisk, zergling}));
	}
	{  // a token extra type isn't named; at most max_types are
		std::vector<point> v;
		add(v, 12, marine, 2, 50, 1000, 1000);
		add(v, 1, medic, 2, 75, 1000, 1010);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1);
		if (c.size() == 1) CHECK(c[0].types == std::vector<int>{marine});
		std::vector<point> w;
		for (int t = 0; t != 5; ++t) add(w, 8, 100 + t, 2, 100, 1000, 1000 + 10 * t);
		c = minimap_clumps::find(w);
		CHECK(c.size() == 1);
		if (c.size() == 1) CHECK((c[0].types == std::vector<int>{100, 101, 102}));  // equal: lower types first
	}
	{  // ties are broken the same way whatever the input order
		std::vector<point> a, b;
		add(a, 8, hydralisk, 2, 125, 1000, 1000);
		add(a, 10, zergling, 1, 100, 1000, 1010);  // equal value 1000 v 1000, hydras have more supply
		add(b, 10, zergling, 1, 100, 1000, 1010);
		add(b, 8, hydralisk, 2, 125, 1000, 1000);
		auto ca = minimap_clumps::find(a), cb = minimap_clumps::find(b);
		CHECK(ca.size() == 1 && cb.size() == 1);
		if (ca.size() == 1 && cb.size() == 1) {
			CHECK((ca[0].types == std::vector<int>{hydralisk, zergling}));
			CHECK(ca[0].types == cb[0].types);
		}
	}
	{  // two armies far apart are two clumps, the bigger first
		std::vector<point> v;
		add(v, 10, marine, 2, 50, 500, 500);
		add(v, 20, zergling, 1, 25, 3000, 3000);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 2);
		if (c.size() == 2) {
			CHECK(c[0].types.front() == marine);  // 20 supply_half v 20: stable, input order
			CHECK(c[1].types.front() == zergling);
		}
		add(v, 2, marine, 2, 50, 500, 520);
		c = minimap_clumps::find(v);
		CHECK(c.size() == 2 && c[0].types.front() == marine && c[0].supply_half == 24);
	}
	{  // a long column (linked unit to unit) is split, so labels sit on its two ends, not
	   // in the empty middle
		std::vector<point> v;
		add(v, 12, marine, 2, 50, 0, 2000, 0);       // 12 Marines at x = 0
		add(v, 20, marine, 2, 50, 100, 2000, 150);   // a thin line of stragglers to x = 2950
		add(v, 12, marine, 2, 50, 3000, 2000, 0);    // 12 Marines at x = 3000
		auto c = minimap_clumps::find(v);
		CHECK(c.size() >= 2);
		bool near_left = false, near_right = false;
		for (auto& k : c) {
			CHECK(k.radius <= minimap_clumps::params{}.max_radius);
			if (k.x < 800) near_left = true;
			if (k.x > 2200) near_right = true;
		}
		CHECK(near_left && near_right);
	}
	{  // a compact ball is not split, however many units
		std::vector<point> v;
		for (int i = 0; i != 100; ++i) v.push_back({1000 + (i % 10) * 12, 1000 + (i / 10) * 12, zergling, 1, 25});
		CHECK(minimap_clumps::find(v).size() == 1);
	}
	{  // malformed input: negative supply/value count as zero, doesn't crash
		std::vector<point> v;
		add(v, 10, marine, -4, -50, 0, 0);
		CHECK(minimap_clumps::find(v).empty());
		add(v, 8, marine, 2, -50, 0, 0);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1);
		if (c.size() == 1) CHECK(c[0].types.front() == marine);
	}
	{  // many units in one spot (all splits degenerate) still make one clump
		std::vector<point> v;
		add(v, 2000, zergling, 1, 25, 64, 64, 0);
		auto c = minimap_clumps::find(v);
		CHECK(c.size() == 1 && c[0].units == 2000);
	}

	// --- badge placement ---
	{  // a free badge goes on its centre
		auto p = minimap_clumps::place({{50, 50, 12, 10}}, 200, 200);
		CHECK(p.size() == 1 && p[0].placed && p[0].x == 44 && p[0].y == 45);
	}
	{  // badges are kept inside the minimap
		auto p = minimap_clumps::place({{0, 0, 12, 10}, {199, 199, 12, 10}}, 200, 200);
		CHECK(p[0].placed && p[0].x == 0 && p[0].y == 0);
		CHECK(p[1].placed && p[1].x == 188 && p[1].y == 190);
	}
	{  // a badge overlapping a bigger one is left out; the first keeps its place
		auto p = minimap_clumps::place({{100, 100, 12, 10}, {106, 104, 12, 10}}, 200, 200);
		CHECK(p[0].placed && p[0].x == 94 && p[0].y == 95);
		CHECK(!p[1].placed);
	}
	{  // badges just clear of each other (with the 1 px gap) are both placed
		auto p = minimap_clumps::place({{100, 100, 12, 10}, {113, 100, 12, 10}}, 200, 200);
		CHECK(p[0].placed && p[1].placed);
		p = minimap_clumps::place({{100, 100, 12, 10}, {112, 100, 12, 10}}, 200, 200);
		CHECK(p[0].placed && !p[1].placed);
	}
	{  // many badges on one spot: only the first
		std::vector<badge> want(12, badge{100, 100, 12, 10});
		auto p = minimap_clumps::place(want, 200, 200);
		int placed = 0;
		for (auto& q : p) placed += q.placed;
		CHECK(placed == 1 && p[0].placed);
	}
	{  // a badge bigger than the minimap, or of no size, isn't placed
		auto p = minimap_clumps::place({{5, 5, 20, 20}, {5, 5, 0, 10}}, 10, 10);
		CHECK(!p[0].placed && !p[1].placed);
	}
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
