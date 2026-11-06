// Behaviour tests for the minimap army clumps (ui/minimap_clumps.h).
// Build and run: cmake --build build --target openbw-minimap-clumps-test && ./build/openbw-minimap-clumps-test
#include "minimap_clumps.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using minimap_clumps::point;
using minimap_clumps::badge;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

enum { zergling = 37, mutalisk = 43, hydralisk = 38, marine = 0, medic = 34, tank = 5 };

// `n` units of a type in a row from (x, y), `step` pixels apart.
static void add(std::vector<point>& v, int n, int type, int value, int x, int y, int step = 8) {
	for (int i = 0; i != n; ++i) v.push_back({x + i * step, y, type, value});
}
// `n` units of a type evenly on a circle of radius r (pixels) around (x, y).
static void ring(std::vector<point>& v, int n, int type, int value, int x, int y, double r) {
	for (int i = 0; i != n; ++i) {
		double t = 6.283185307 * i / n;
		v.push_back({x + (int)std::lround(r * std::cos(t)), y + (int)std::lround(r * std::sin(t)), type, value});
	}
}

// The scenarios below are written for a 500 minerals + gas threshold; the default's own
// value is checked first.
static const minimap_clumps::params P = [] { minimap_clumps::params p; p.min_cost = 500; return p; }();

int main() {
	{  // the default threshold: 20 Zerglings (500) are too few for an icon, 40 bunched are enough
		std::vector<point> few, enough;
		add(few, 20, zergling, 25, 1000, 1000, 4);
		add(enough, 40, zergling, 25, 1000, 1000, 4);
		CHECK(minimap_clumps::find(few).empty());
		CHECK(minimap_clumps::find(enough).size() == 1);
	}

	{  // nothing, or too little of a type: no icons
		CHECK(minimap_clumps::find({}, P).empty());
		std::vector<point> v;
		add(v, 2, mutalisk, 200, 1000, 1000);  // 400
		add(v, 12, zergling, 25, 1000, 1010);  // 300
		CHECK(minimap_clumps::find(v, P).empty());
	}
	{  // a stack of Mutalisks on one spot is a Mutalisk icon at the stack
		std::vector<point> v;
		add(v, 11, mutalisk, 200, 1000, 1000, 0);
		auto g = minimap_clumps::find_groups(v, P);
		CHECK(g.size() == 1);
		if (g.size() == 1) CHECK(g[0].type == mutalisk && g[0].cost == 2200 && g[0].units == 11 && g[0].radius == 0);
		auto l = minimap_clumps::find(v, P);
		CHECK(l.size() == 1);
		if (l.size() == 1) CHECK(l[0].types == std::vector<int>{mutalisk} && l[0].x == 1000 && l[0].y == 1000);
	}
	{  // types are judged on their own cost, not the mix: 8 Mutalisks with 12 Zerglings
	   // show the Mutalisk only, though there are more Zerglings
		std::vector<point> v;
		add(v, 12, zergling, 25, 1000, 1000);
		add(v, 8, mutalisk, 200, 1000, 1010);
		auto l = minimap_clumps::find(v, P);
		CHECK(l.size() == 1);
		if (l.size() == 1) CHECK(l[0].types == std::vector<int>{mutalisk});
	}
	{  // 20 Zerglings under 8 Mutalisks: both earn icons, sharing one row, Mutalisk first
		std::vector<point> v;
		add(v, 20, zergling, 25, 1000, 1000);
		add(v, 8, mutalisk, 200, 1000, 1010);
		auto l = minimap_clumps::find(v, P);
		CHECK(l.size() == 1);
		if (l.size() == 1) CHECK((l[0].types == std::vector<int>{mutalisk, zergling}));
	}
	{  // the same forces far apart are two rows, the costlier first
		std::vector<point> v;
		add(v, 20, zergling, 25, 3000, 3000);
		add(v, 8, mutalisk, 200, 500, 500);
		auto l = minimap_clumps::find(v, P);
		CHECK(l.size() == 2);
		if (l.size() == 2) {
			CHECK(l[0].types == std::vector<int>{mutalisk} && l[0].x < 1000);
			CHECK(l[1].types == std::vector<int>{zergling} && l[1].x > 2500);
		}
	}
	{  // enough cost but thinly scattered: no icon. The same units bunched: an icon.
		std::vector<point> thin, bunched;
		ring(thin, 24, zergling, 25, 2000, 2000, 5.5 * 32);  // 600 over ~95 square tiles
		ring(bunched, 24, zergling, 25, 2000, 2000, 2 * 32);
		CHECK(minimap_clumps::find(thin, P).empty());
		CHECK(minimap_clumps::find(bunched, P).size() == 1);
	}
	{  // density uses a minimum spread, so a tiny force stacked on one pixel isn't "infinitely
	   // dense": it still needs the cost
		std::vector<point> v;
		add(v, 2, mutalisk, 200, 1000, 1000, 0);
		CHECK(minimap_clumps::find(v, P).empty());
		minimap_clumps::params p = P;
		p.min_cost = 300;
		auto g = minimap_clumps::find_groups(v, p);
		CHECK(g.size() == 1);
		if (g.size() == 1) CHECK(std::abs(g[0].density - 400 / (3.14159265 * 4)) < 0.01);
	}
	{  // at most max_types icons in one row; the rest start their own row
		std::vector<point> v;
		for (int t = 0; t != 5; ++t) add(v, 10, 100 + t, 100 - t, 1000, 1000 + 4 * t);
		auto l = minimap_clumps::find(v, P);
		CHECK(l.size() == 2);
		if (l.size() == 2) {
			CHECK((l[0].types == std::vector<int>{100, 101, 102}));
			CHECK((l[1].types == std::vector<int>{103, 104}));
		}
	}
	{  // equal costs are ordered by type, whatever the input order
		std::vector<point> a, b;
		add(a, 8, hydralisk, 125, 1000, 1000);
		add(a, 40, zergling, 25, 1000, 1010, 2);
		add(b, 40, zergling, 25, 1000, 1010, 2);
		add(b, 8, hydralisk, 125, 1000, 1000);
		auto la = minimap_clumps::find(a, P), lb = minimap_clumps::find(b, P);
		CHECK(la.size() == 1 && lb.size() == 1);
		if (la.size() == 1 && lb.size() == 1) {
			CHECK((la[0].types == std::vector<int>{zergling, hydralisk}));  // 37 < 38
			CHECK(la[0].types == lb[0].types);
		}
	}
	{  // a long column is split: icons sit on its two dense ends, not in the empty middle,
	   // and the thin line between them earns none
		std::vector<point> v;
		add(v, 12, marine, 50, 0, 2000, 0);
		add(v, 20, marine, 50, 100, 2000, 150);
		add(v, 12, marine, 50, 3000, 2000, 0);
		auto l = minimap_clumps::find(v, P);
		bool left = false, right = false, middle = false;
		for (auto& k : l) {
			if (k.x < 800) left = true;
			else if (k.x > 2200) right = true;
			else middle = true;
		}
		CHECK(left && right && !middle);
	}
	{  // a compact ball is one icon, however many units
		std::vector<point> v;
		for (int i = 0; i != 100; ++i) v.push_back({1000 + (i % 10) * 12, 1000 + (i / 10) * 12, zergling, 25});
		CHECK(minimap_clumps::find(v, P).size() == 1);
		std::vector<point> w;
		add(w, 2000, zergling, 25, 64, 64, 0);
		auto g = minimap_clumps::find_groups(w, P);
		CHECK(g.size() == 1 && g[0].units == 2000 && g[0].cost == 50000);
	}
	{  // a big blob split into parts by the diameter rule is still one icon
		std::vector<point> v;
		for (int i = 0; i != 60; ++i) v.push_back({2000 + (i % 8) * 40, 2000 + (i / 8) * 40, zergling, 25});  // 9 x 9 tiles
		minimap_clumps::params p = P;
		p.merge_distance = 10 * 32;  // the viewer uses about an icon's width
		auto l = minimap_clumps::find(v, p);
		CHECK(l.size() == 1);
		if (l.size() == 1) CHECK(l[0].types == std::vector<int>{zergling} && l[0].cost == 1500);
	}
	{  // groups never exceed the maximum diameter: a wide force is cut into parts that fit,
	   // each earning its own icon if it qualifies
		std::vector<point> v;
		for (int i = 0; i != 200; ++i) v.push_back({1000 + (i % 25) * 40, 1000 + (i / 25) * 40, marine, 50});  // 31 x 10 tiles
		auto g = minimap_clumps::find_groups(v, P);
		CHECK(g.size() >= 4);
		for (auto& k : g) CHECK(k.diameter <= minimap_clumps::params{}.max_diameter);
		std::vector<point> fits;
		ring(fits, 24, zergling, 25, 2000, 2000, 4.5 * 32);  // 9 tiles across: one group
		minimap_clumps::params p = P;
		p.min_density = 0;
		auto f = minimap_clumps::find_groups(fits, p);
		CHECK(f.size() == 1);
		if (f.size() == 1) CHECK(f[0].cost == 600 && f[0].diameter <= 9 * 32 + 1);
	}
	{  // a force whose parts are each too small after the cut shows nothing: the cap wins
		std::vector<point> v;
		add(v, 12, marine, 50, 0, 0, 0);
		add(v, 12, marine, 50, 15 * 32, 0, 0);  // 15 tiles apart, linked by nothing: two groups
		CHECK(minimap_clumps::find(v, P).size() == 2);
		std::vector<point> w;
		add(w, 6, marine, 50, 0, 0, 0);
		add(w, 6, marine, 50, 11 * 32, 0, 0);
		add(w, 1, marine, 50, 5 * 32, 0, 0);  // links the two (within 6 tiles) but spans 11
		CHECK(minimap_clumps::find(w, P).empty());
	}
	{  // malformed values count as zero; huge ones don't overflow
		std::vector<point> v;
		add(v, 10, marine, -500, 0, 0);
		CHECK(minimap_clumps::find(v, P).empty());
		std::vector<point> w;
		add(w, 1000, marine, 1 << 30, 0, 0, 0);
		auto l = minimap_clumps::find(w, P);
		CHECK(l.size() == 1 && l[0].cost == 1000LL * (1 << 30));
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
