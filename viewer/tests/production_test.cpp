// Behaviour tests for the production list (ui/production.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/production_test.cpp -o /tmp/production_test && /tmp/production_test
#include "production.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using production::item;
using production::kind;

static item zealot(uint32_t gateway, int remaining, int player = 0) {
	item i;
	i.player = player, i.what = kind::unit, i.type = 65, i.source = gateway, i.remaining = remaining, i.total = 600;
	return i;
}

int main() {
	{
		// Grouping: one row per (kind, type, level), counted, soonest first, per player.
		std::vector<item> items = {zealot(1, 300), zealot(2, 120), zealot(3, 50, 1)};
		item ups;
		ups.what = kind::upgrade, ups.type = 1, ups.level = 2, ups.source = 9, ups.remaining = 60, ups.total = 4000;
		items.push_back(ups);
		auto g = production::grouped(items, 0);
		CHECK(g.size() == 2);
		CHECK(g[0].what == kind::upgrade && g[0].level == 2 && g[0].remaining == 60);
		CHECK(g[1].type == 65 && g[1].count == 2 && g[1].remaining == 120 && g[1].progress > 0.79 && g[1].progress < 0.81);
		CHECK(production::grouped(items, 1).size() == 1 && production::grouped(items, 5).empty());
		// Nonsense times are clamped rather than shown.
		item bad = zealot(4, -7);
		bad.total = 0;
		auto gb = production::grouped({bad}, 0);
		CHECK(gb.size() == 1 && gb[0].remaining == 0 && gb[0].progress == 0);
	}
	{
		production::tracker t;
		// A Zealot counts down and finishes: gone the frame after its last.
		for (int f = 100; f <= 102; ++f) t.update(f, {zealot(1, 102 - f)});
		t.update(103, {});
		auto r = t.recent(0, 103, 24 * 60);
		CHECK(r.size() == 1 && r[0].frame == 103 && r[0].type == 65);
		// Cancelled with time left: not a completion.
		t.update(104, {zealot(2, 200)});
		t.update(105, {});
		CHECK(t.recent(0, 105, 24 * 60).size() == 1);
		// The same Gateway's next Zealot (same source, new item) finishing later is another one.
		t.update(106, {zealot(1, 1)});
		t.update(107, {zealot(1, 0)});
		t.update(108, {});
		CHECK(t.recent(0, 108, 24 * 60).size() == 2 && t.recent(0, 108, 24 * 60)[0].frame == 108);
		// A queue: the next Zealot begins on the frame the last one finished.
		t.update(109, {zealot(5, 1)});
		t.update(110, {zealot(5, 600)});
		CHECK(t.recent(0, 110, 24 * 60).size() == 3 && t.recent(0, 110, 24 * 60)[0].frame == 110);
		t.update(111, {zealot(5, 599)});  // counting down: not another completion
		CHECK(t.recent(0, 111, 24 * 60).size() == 3);
		t.update(112, {});  // cancelled with time left
		// Only the window: completions older than it, or after the frame shown, are left out.
		CHECK(t.recent(0, 108, 3).size() == 1 && t.recent(0, 103, 24 * 60).size() == 1 && t.recent(0, 102, 24 * 60).empty());
		CHECK(t.recent(0, 112, 24 * 60).size() == 3);
		CHECK(t.recent(1, 112, 24 * 60).empty());
		// A seek back (a jump) is not read as completions, and simulating the same stretch
		// again finds the same completion once.
		t.update(50, {});
		t.update(101, {zealot(1, 2)});  // jump: nothing compared
		t.update(102, {zealot(1, 1)});
		t.update(103, {});
		CHECK(t.recent(0, 112, 24 * 60).size() == 3);
		// A jump away while an item is about to finish is not a completion either.
		t.update(200, {zealot(7, 1)});
		t.update(400, {});
		CHECK(t.recent(0, 400, 1000).size() == 3);
		t.clear();
		CHECK(t.recent(0, 1000, 1000).empty() && t.now().empty());
	}
	{
		// Completions grouped: one row per (kind, type, level), counted, newest group first.
		using production::finished;
		std::vector<finished> done = {{100, 0, kind::unit, 64, 0}, {130, 0, kind::unit, 64, 0}, {120, 0, kind::building, 156, 0},
			{90, 0, kind::upgrade, 1, 1}, {95, 0, kind::upgrade, 1, 2}};
		auto g = production::grouped(done);
		CHECK(g.size() == 4);
		CHECK(g[0].type == 64 && g[0].count == 2 && g[0].newest == 130);
		CHECK(g[1].what == kind::building && g[1].newest == 120);
		CHECK(g[2].level == 2 && g[3].level == 1);  // levels are distinct rows
		CHECK(production::grouped(std::vector<finished>{}).empty());
	}
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("production: all passed\n");
	return 0;
}
