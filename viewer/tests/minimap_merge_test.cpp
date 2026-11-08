// Behaviour tests for merging close minimap dots (ui/minimap_merge.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/minimap_merge_test.cpp -o /tmp/minimap_merge_test && /tmp/minimap_merge_test
#include "minimap_merge.h"

#include <cstdio>
#include <limits>

using namespace minimap_merge;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
	const double r = 2;
	// Nothing in, nothing out.
	CHECK(merge({}).empty());

	// Three dots almost on one spot become one dot at their centroid, grown by the cube root of the count.
	auto m = merge({{10, 10, r, 1}, {10.5, 10, r, 1}, {10, 10.5, r, 1}});
	CHECK(m.size() == 1);
	CHECK(m[0].n == 3 && m[0].key == 1 && m[0].first == 0);
	CHECK(near(m[0].x, (10 + 10.5 + 10) / 3));
	CHECK(near(m[0].y, (10 + 10 + 10.5) / 3));
	CHECK(near(m[0].r, r * std::cbrt(3.0)) && near(m[0].r1, r));

	// Two dots on one spot stay two: a pair is not a crowd.
	m = merge({{10, 10, r, 1}, {10, 10, r, 1}});
	CHECK(m.size() == 2 && m[0].n == 1 && m[1].n == 1 && near(m[0].r, r) && near(m[0].r1, r));

	// Only very close dots merge: touching, or overlapping by half, is not enough.
	m = merge({{10, 10, r, 1}, {10 + 2 * r, 10, r, 1}, {10 + 4 * r, 10, r, 1}});
	CHECK(m.size() == 3);
	m = merge({{10, 10, r, 1}, {10 + r, 10, r, 1}, {10, 10 + r, r, 1}});
	CHECK(m.size() == 3);
	// A column of closely spaced dots does not chain into one big dot.
	std::vector<dot> column;
	for (int i = 0; i != 20; ++i) column.push_back({10, 10 + i * r * 0.9, r, 1});
	m = merge(column);
	CHECK(m.size() >= 10);
	for (auto& b : m) CHECK(b.r <= r * std::cbrt(4.0) + 1e-9);

	// Different kinds (another player, another type) never merge, even on one spot.
	m = merge({{10, 10, r, 1}, {10, 10, r, 2}, {10, 10, r, 1}, {10, 10, r, 2}, {10, 10, r, 1}});
	CHECK(m.size() == 3);
	int ones = 0, twos = 0;
	for (auto& b : m) (b.key == 1 ? ones : twos) += b.n;
	CHECK(ones == 3 && twos == 2);
	for (auto& b : m) if (b.key == 1) CHECK(b.n == 3 && b.first == 0);

	// A big stack grows by the cube root of its count, capped at max_scale times a single dot.
	std::vector<dot> stack(40, dot{50, 50, r, 7});
	m = merge(stack);
	CHECK(m.size() == 1 && m[0].n == 40 && near(m[0].r, std::cbrt(40.0) * r));
	CHECK(near(merge(std::vector<dot>(100, dot{50, 50, r, 7}))[0].r, 4 * r));
	params p;
	p.max_scale = 2;
	CHECK(near(merge(stack, p)[0].r, 2 * r));
	p.min_count = 50;  // a threshold above the stack: every dot stays
	CHECK(merge(stack, p).size() == 40);

	// Merging is transitive through growth: a ball whose parts start just out of reach of each
	// other still ends as one dot once the inner ones have merged.
	std::vector<dot> ball;
	for (int i = 0; i != 9; ++i) ball.push_back({100 + (i % 3) * 0.5, 100 + (i / 3) * 0.5, r, 3});
	m = merge(ball);
	CHECK(m.size() == 1 && m[0].n == 9);

	// Output order follows the first dot of each, so the caller's draw order holds.
	m = merge({{0, 0, r, 5}, {50, 50, r, 1}, {50, 50, r, 1}, {50, 50, r, 1}, {90, 90, r, 2}});
	CHECK(m.size() == 3 && m[0].first == 0 && m[1].first == 1 && m[2].first == 4 && m[1].n == 3);

	// Malformed input: positions that are not numbers are dropped; bad radii count as 0.
	const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
	m = merge({{nan, 10, r, 1}, {10, inf, r, 1}, {10, 10, -3, 1}, {20, 20, nan, 1}});
	CHECK(m.size() == 2);
	for (auto& b : m) CHECK(b.r == 0 && b.n == 1);
	p = params();
	p.max_scale = nan;
	m = merge(stack, p);
	CHECK(m.size() == 1 && std::isfinite(m[0].r) && m[0].r >= r - 1e-9);
	p = params();
	p.min_count = -5;  // never merges pairs or single dots into "groups" of one
	m = merge({{10, 10, r, 1}}, p);
	CHECK(m.size() == 1 && m[0].n == 1);

	// With a cost scale, size follows the group's total cost, the same scale for every type:
	// 12 Mutalisks (2400) are much bigger than 12 Zerglings (300), which are bigger than one
	// unit's dot of the reference cost; equal totals give equal sizes whatever the count.
	{
		params c;
		c.unit_r = 3, c.ref_cost = 150;
		std::vector<dot> lings(12, dot{10, 10, 2, 1, 25}), mutas(12, dot{50, 50, 3.5, 2, 200});
		auto ml = merge(lings, c), mm = merge(mutas, c);
		CHECK(ml.size() == 1 && mm.size() == 1);
		CHECK(near(ml[0].r, 3 * std::cbrt(300.0 / 150)));
		CHECK(near(mm[0].r, 3 * std::cbrt(16.0)));
		// Nine Wraiths (350 each) are about twice one Wraith's dot across, not three times.
		std::vector<dot> wraiths(9, dot{10, 10, 3 * 1.3, 6, 350});
		auto mw = merge(wraiths, c);
		CHECK(mw.size() == 1 && mw[0].r / (3 * 1.3) > 1.9 && mw[0].r / (3 * 1.3) < 2.3);
		// A huge army reaches the cap.
		CHECK(near(merge(std::vector<dot>(40, dot{0, 0, 3, 9, 400}), c)[0].r, 3 * 4));
		CHECK(mm[0].r > ml[0].r);
		std::vector<dot> a(4, dot{0, 0, 2, 3, 150}), b(8, dot{90, 90, 2, 4, 75});
		CHECK(near(merge(a, c)[0].r, merge(b, c)[0].r));
		// Three Zerglings (75): sized by their cost; on a scale where that would be smaller
		// than one Zergling's dot, they keep a Zergling's size.
		std::vector<dot> three(3, dot{10, 10, 2, 1, 25});
		auto m3 = merge(three, c);
		CHECK(m3.size() == 1 && m3[0].n == 3 && near(m3[0].r, 3 * std::cbrt(75.0 / 150)));
		params small = c;
		small.unit_r = 1.5;
		auto m3s = merge(three, small);
		CHECK(m3s.size() == 1 && near(m3s[0].r, 2));
		// Costs that are not numbers or not positive count as nothing.
		std::vector<dot> bad = {{0, 0, 2, 5, nan}, {0, 0, 2, 5, -100}, {0, 0, 2, 5, inf}};
		auto mb = merge(bad, c);
		CHECK(mb.size() == 1 && near(mb[0].r, 2));
		// A broken scale falls back to sizing by count.
		c.unit_r = nan;
		CHECK(near(merge(three, c)[0].r, 2 * std::cbrt(3.0)));
		c.unit_r = 3, c.ref_cost = 0;
		CHECK(near(merge(three, c)[0].r, 2 * std::cbrt(3.0)));
		// A growth that is not a sensible exponent falls back to the cube root.
		c.ref_cost = 150, c.growth = std::numeric_limits<double>::quiet_NaN();
		CHECK(near(merge(three, c)[0].r, 3 * std::cbrt(75.0 / 150)));
		c.growth = 0.5;
		CHECK(near(merge(three, small)[0].r, 2));
		CHECK(near(merge(lings, c)[0].r, 3 * std::sqrt(2.0)));
	}

	// Many dots spread out: each its own, and the merge stays fast enough per frame.
	std::vector<dot> spread;
	for (int i = 0; i != 400; ++i) spread.push_back({(double)(i % 20) * 10, (double)(i / 20) * 10, r, i % 4});
	CHECK(merge(spread).size() == 400);

	{
		// Transports grow with their cargo, on the merge scale, within limits.
		using minimap_merge::cargo_scale;
		CHECK(cargo_scale(200, 0) == 1);                                       // empty
		CHECK(std::fabs(cargo_scale(200, 400) - std::cbrt(3.0)) < 1e-9);       // eight Marines
		CHECK(cargo_scale(200, 200) < cargo_scale(200, 400));                  // more is bigger
		CHECK(cargo_scale(100, 1e9) == 2);                                     // capped
		CHECK(cargo_scale(0, 400) == 1 && cargo_scale(200, -5) == 1);          // nonsense
		CHECK(cargo_scale(std::nan(""), 400) == 1 && cargo_scale(200, std::nan("")) == 1);
		CHECK(cargo_scale(200, 400, 7, 0) == 1);                               // bad growth, cap
	}
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("minimap_merge: all passed\n");
	return 0;
}
