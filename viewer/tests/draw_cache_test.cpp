// Behaviour tests for the redraw cache (ui/draw_cache.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/draw_cache_test.cpp -o /tmp/draw_cache_test && /tmp/draw_cache_test
#include "draw_cache.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

struct key {
	int frame = 0, x = 0, y = 0;
	bool fog = false;
	bool operator==(const key& o) const { return frame == o.frame && x == o.x && y == o.y && fog == o.fog; }
};

int main() {
	draw_cache::cache<key> c;
	key k;
	// Nothing is cached before the first draw, even for a default key.
	CHECK(!c.valid());
	CHECK(c.needs_redraw(k));
	// The same inputs reuse the surface, however often asked.
	for (int i = 0; i != 100; ++i) CHECK(!c.needs_redraw(k));
	// Any single input changing redraws once, then settles.
	key a = k; a.frame = 1;
	CHECK(c.needs_redraw(a));
	CHECK(!c.needs_redraw(a));
	key b = a; b.x = 5;
	CHECK(c.needs_redraw(b));
	key d = b; d.y = -3;
	CHECK(c.needs_redraw(d));
	key e = d; e.fog = true;
	CHECK(c.needs_redraw(e));
	CHECK(!c.needs_redraw(e));
	// Going back to an earlier key is a change too (only the last build is kept).
	CHECK(c.needs_redraw(d));
	// Rewinding to an earlier game frame redraws.
	key f = d; f.frame = 0;
	CHECK(c.needs_redraw(f));
	// Invalidation (surface recreated) forces a redraw with unchanged inputs, once.
	c.invalidate();
	CHECK(!c.valid());
	CHECK(c.needs_redraw(f));
	CHECK(c.valid());
	CHECK(!c.needs_redraw(f));
	// After invalidation a default key still redraws.
	c.invalidate();
	CHECK(c.needs_redraw(key{}));

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("ok\n");
	return 0;
}
