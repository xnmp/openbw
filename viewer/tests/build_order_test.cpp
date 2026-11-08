// Behaviour tests for the build order log (ui/build_order.h) and key chords (ui/key_chord.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/build_order_test.cpp -o /tmp/build_order_test && /tmp/build_order_test
#include "build_order.h"
#include "key_chord.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	{
		build_order::log l;
		CHECK(l.upto(0, 100000).empty());
		// Starts come back in time order, per player, up to the frame on screen.
		l.add(7, 3046, 0, 131, 10);   // Hatchery
		l.add(5, 1915, 0, 142, 9);    // Spawning Pool
		l.add(9, 2551, 1, 131, 11);
		l.add(8, 3529, 0, 149, 13);   // Extractor
		auto p0 = l.upto(0, 100000);
		CHECK(p0.size() == 3 && p0[0].type == 142 && p0[0].supply == 9 && p0[1].type == 131 && p0[2].type == 149);
		CHECK(l.upto(1, 100000).size() == 1);
		CHECK(l.upto(0, 3045).size() == 1 && l.upto(0, 3046).size() == 2);  // "up to and including"
		CHECK(l.upto(0, 0).empty() && l.upto(3, 100000).empty());
		// Simulating the same stretch again (after seeking back) reports the same starts again:
		// they are kept once, and the first report stands.
		l.add(5, 1915, 0, 142, 9);
		l.add(5, 1999, 0, 142, 12);
		CHECK(l.upto(0, 100000).size() == 3 && l.upto(0, 100000)[0].frame == 1915);
		CHECK(l.has(5, 142) && !l.has(5, 143));
		// The same unit becoming another building (a Hatchery's Lair) is another start.
		l.add(7, 4361, 0, 132, 17);
		CHECK(l.upto(0, 100000).size() == 4 && l.upto(0, 100000).back().type == 132);
		// Two starts on one frame keep a stable order.
		l.add(21, 5000, 1, 143, 20);
		l.add(20, 5000, 1, 143, 20);
		auto p1 = l.upto(1, 100000);
		CHECK(p1.size() == 3 && p1[1].frame == 5000 && p1[2].frame == 5000);
		// Nonsense is ignored or clamped rather than shown.
		l.add(30, -5, 0, 142, 9);
		l.add(31, 10, -1, 142, 9);
		l.add(32, 10, 1, 142, -4);
		CHECK(l.upto(0, 100000).size() == 4 && l.upto(1, 100000).front().supply == 0);
		l.clear();
		CHECK(l.upto(0, 100000).empty() && !l.has(5, 142));
		// A long game: thousands of starts stay quick to list.
		for (uint32_t i = 0; i != 20000; ++i) l.add(i, (int)i, (int)(i % 2), 142, 9);
		CHECK(l.upto(0, 1 << 30).size() == 10000 && l.upto(1, 99).size() == 50);
	}
	{
		using key_chord::result;
		const int alt_key = 0x400000E2;
		key_chord::reader c('m');
		// Alt+M then B: the chord; neither key does what it would alone.
		CHECK(c.key(alt_key, true, 0) == result::none);  // Alt by itself: nothing yet
		CHECK(c.key('m', true, 0.1) == result::started && c.pending(0.2));
		CHECK(c.key('b', false, 0.5) == result::completed && c.second_key() == 'b' && !c.pending(0.6));
		// Alt still held for the second key works too, and so does a capital.
		CHECK(c.key('m', true, 1) == result::started);
		CHECK(c.key('B', true, 1.2) == result::completed && c.second_key() == 'b');
		// M without Alt is just M; B without the prefix is just B.
		CHECK(c.key('m', false, 2) == result::none && c.key('b', false, 2.1) == result::none);
		// Modifiers between the prefix and the key are ignored.
		CHECK(c.key('m', true, 3) == result::started);
		CHECK(c.key(0x400000E1, false, 3.1) == result::started);  // Shift
		CHECK(c.key('b', false, 3.2) == result::completed);
		// Too slow: the chord is over, and the late key is an ordinary key.
		CHECK(c.key('m', true, 4) == result::started);
		CHECK(!c.pending(6.5) && c.key('b', false, 6.5) == result::none);
		// Esc ends it, swallowed.
		CHECK(c.key('m', true, 7) == result::started);
		CHECK(c.key(27, false, 7.1) == result::cancelled && !c.pending(7.2));
		// A clock that goes back does not leave a chord pending forever.
		CHECK(c.key('m', true, 10) == result::started);
		CHECK(!c.pending(5));
	}
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("build_order: all passed\n");
	return 0;
}
