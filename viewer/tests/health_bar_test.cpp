// Behaviour tests for the health bar rules (ui/health_bar.h).
// Build and run: cmake --build build --target openbw-health-bar-test && ./build/openbw-health-bar-test
#include "health_bar.h"

#include <cstdio>

using namespace health_bar;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	{  // width: game normalisation, then zoom
		CHECK(bar_width(19, 1.0) == 19);
		CHECK(bar_width(0, 1.0) == 19);    // degenerate sizes get the minimum
		CHECK(bar_width(-5, 1.0) == 19);
		CHECK(bar_width(20, 1.0) == 19);   // 20 -> 19 (multiple of 3, plus 1)
		CHECK(bar_width(19, 2.0) == 38);
		CHECK(bar_width(19, 0.0) == 1);    // never zero-width
	}
	{  // pips: grow with hp, bounded, never narrower than 4 px
		CHECK(segment_count(40, 19) == 2);
		CHECK(segment_count(100, 19) == 4);
		CHECK(segment_count(400, 60) == 12);
		CHECK(segment_count(1500, 200) == 12);   // buildings do not become tick soup
		CHECK(segment_count(1 << 30, 200) == 12);
		CHECK(segment_count(0, 19) == 2);
		CHECK(segment_count(-10, 19) == 2);
		CHECK(segment_count(1500, 19) == 4);     // 19 px fits four 4 px pips
		CHECK(segment_count(1500, 3) == 1);
		CHECK(segment_count(1500, 0) == 1);
		for (int hp = 1; hp < 3000; hp += 7) CHECK(segment_count(hp, 19) >= segment_count(hp - 1 > 0 ? hp - 1 : 1, 19));
	}
	{  // fill width: empty only at 0, full only at 1, monotonic
		CHECK(fill_width(0, 20) == 0);
		CHECK(fill_width(-1, 20) == 0);
		CHECK(fill_width(0.0001, 20) == 1);
		CHECK(fill_width(0.999, 20) == 19);
		CHECK(fill_width(1, 20) == 20);
		CHECK(fill_width(7, 20) == 20);
		CHECK(fill_width(0.5, 20) == 10);
		double nan = std::nan("");
		CHECK(fill_width(nan, 20) == 0);
		int prev = 0;
		for (int i = 0; i <= 100; ++i) {
			int f = fill_width(i / 100.0, 37);
			CHECK(f >= prev);
			prev = f;
		}
	}
	{  // ticks sit strictly inside the bar, ordered
		for (int segs = 2; segs <= 12; ++segs) {
			int prev = 0;
			for (int i = 1; i < segs; ++i) {
				int x = tick_x(i, segs, 57);
				CHECK(x > prev && x < 57);
				prev = x;
			}
		}
	}
	{  // colour: green when healthy, red when nearly dead, smooth between
		CHECK(hp_color(1.0) == hp_color(0.7));
		CHECK(hp_color(0.0).r > hp_color(0.0).g);
		CHECK(hp_color(1.0).g > hp_color(1.0).r);
		CHECK(hp_color(5.0) == hp_color(1.0) && hp_color(-5.0) == hp_color(0.0));
		CHECK(hp_color(std::nan("")).r >= 0);
		rgb a = hp_color(0.50), b = hp_color(0.51);
		CHECK(std::abs(a.r - b.r) < 8 && std::abs(a.g - b.g) < 8);
		CHECK(shade({100, 100, 100}, 100) == rgb({255, 255, 255}));
		CHECK(shade({100, 100, 100}, -100) == rgb({0, 0, 0}));
		CHECK(shade({100, 100, 100}, 0) == rgb({100, 100, 100}));
	}
	{  // layout: rows ordered shield, hp, energy; no overlap; fit in total height
		for (int mask = 0; mask < 4; ++mask) {
			for (double s : {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 8.0}) {
				bool sh = mask & 1, en = mask & 2;
				auto l = layout(sh, en, s);
				CHECK(l.hp.h >= 3 && l.hp.h <= 10);
				CHECK((l.shield.h > 0) == sh && (l.energy.h > 0) == en);
				if (sh) CHECK(l.shield.y + l.shield.h < l.hp.y);
				if (en) CHECK(l.hp.y + l.hp.h < l.energy.y);
				int last = en ? l.energy.y + l.energy.h : l.hp.y + l.hp.h;
				CHECK(l.total_h == last + l.pad);
				CHECK(l.hp.y >= l.pad);
			}
		}
		CHECK(layout(false, false, 1.0).hp.h == 3);
		CHECK(layout(false, false, 4.0).hp.h == 10);
	}
	{  // row shading: lit top edge, darker toward the bottom
		CHECK(row_shade(0, 5) > row_shade(1, 5));
		CHECK(row_shade(1, 5) > row_shade(4, 5));
		CHECK(row_shade(0, 1) == 0);
	}
	{
		CHECK(bar_alpha(true) < bar_alpha(false));
	}
	if (failures == 0) std::printf("all passed\n");
	return failures ? 1 : 0;
}
