// Behaviour tests for following a live replay (ui/live_stream.h) and playing it from a queue (ui/live_buffer.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/live_stream_test.cpp -o /tmp/live_stream_test && /tmp/live_stream_test
#include "live_stream.h"
#include "live_buffer.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using bytes = std::vector<uint8_t>;

static void u32(bytes& b, uint32_t v) { for (int i = 0; i != 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static bytes header(uint32_t map_size = 3) {
	bytes b;
	u32(b, live_stream::magic);
	u32(b, live_stream::version);
	for (size_t i = 0; i != live_stream::game_info_size; ++i) b.push_back((uint8_t)i);
	u32(b, map_size);
	for (uint32_t i = 0; i != map_size; ++i) b.push_back(0xAB);
	return b;
}
static bytes record(int32_t frame, bytes cmds) {
	bytes b;
	u32(b, (uint32_t)frame);
	b.push_back((uint8_t)cmds.size());
	b.insert(b.end(), cmds.begin(), cmds.end());
	return b;
}
static bytes cat(std::initializer_list<bytes> parts) {
	bytes r;
	for (auto& p : parts) r.insert(r.end(), p.begin(), p.end());
	return r;
}

int main() {
	using live_stream::reader;
	// A whole file at once: setup, frames' commands in replay form, and the end.
	{
		bytes file = cat({header(), record(0, {}), record(5, {1, 0x2B, 0}), record(5, {0, 0x1A, 3}), record(9, {}), record(-1, {})});
		reader r;
		r.feed(file.data(), file.size());
		CHECK(!r.failed() && r.has_setup() && r.ended());
		CHECK(r.game_info()[10] == 10 && r.map().size() == 3 && r.map()[0] == 0xAB);
		CHECK(r.known_frame() == 9);
		// Records with commands come out as they were (replay form); empty ones only move the edge.
		CHECK(r.take_actions() == cat({record(5, {1, 0x2B, 0}), record(5, {0, 0x1A, 3})}));
		CHECK(r.take_actions().empty());
	}
	// The same file in dribs and drabs, one byte at a time: the same result, and nothing half-read.
	{
		bytes file = cat({header(), record(2, {1, 0x2B, 0}), record(7, {}), record(9, {0, 0x1A, 1}), record(-1, {})});
		reader r;
		bytes got;
		int max_known = -1;
		for (size_t i = 0; i != file.size(); ++i) {
			r.feed(&file[i], 1);
			CHECK(!r.failed());
			if (i + 1 < header().size()) CHECK(!r.has_setup());
			CHECK(r.known_frame() >= max_known);
			max_known = r.known_frame();
			auto a = r.take_actions();
			got.insert(got.end(), a.begin(), a.end());
		}
		CHECK(r.has_setup() && r.ended() && r.known_frame() == 9);  // the end completes frame 9
		CHECK(got == cat({record(2, {1, 0x2B, 0}), record(9, {0, 0x1A, 1})}));
	}
	// A game still going: the edge moves as frames arrive; no end yet.
	{
		reader r;
		bytes a = cat({header(), record(1, {}), record(3, {0, 0x1A, 1})});
		r.feed(a.data(), a.size());
		CHECK(r.known_frame() == 1 && !r.ended());  // frame 3 has commands, but may have more
		bytes b = record(3, {1, 0x1A, 2});
		r.feed(b.data(), b.size());
		CHECK(r.known_frame() == 1);
		CHECK(r.take_actions() == cat({record(3, {0, 0x1A, 1}), record(3, {1, 0x1A, 2})}));
		bytes c = record(4, {});
		r.feed(c.data(), 3);  // cut short: not counted yet
		CHECK(r.known_frame() == 1);
		r.feed(c.data() + 3, c.size() - 3);
		CHECK(r.known_frame() == 4);
		// Commands for a frame already marked complete would have been missed: an error.
		bytes late = record(4, {0, 0x1A, 3});
		r.feed(late.data(), late.size());
		CHECK(r.failed());
	}
	// Malformed files fail with a reason instead of being read as something else.
	{
		bytes bad = header();
		bad[0] ^= 1;
		reader r;
		r.feed(bad.data(), bad.size());
		CHECK(r.failed() && !r.has_setup() && !r.error().empty());
		bytes wrong_version = header();
		wrong_version[4] = 9;
		reader r2;
		r2.feed(wrong_version.data(), wrong_version.size());
		CHECK(r2.failed());
		bytes huge_map = header(0);
		huge_map[8 + live_stream::game_info_size + 3] = 0x7F;  // map size 0x7F000000
		reader r3;
		r3.feed(huge_map.data(), huge_map.size());
		CHECK(r3.failed());
		reader r4;
		bytes no_map = header(0);
		r4.feed(no_map.data(), no_map.size());
		CHECK(r4.failed());
		bytes back = cat({header(), record(8, {}), record(6, {})});
		reader r5;
		r5.feed(back.data(), back.size());
		CHECK(r5.failed() && r5.known_frame() == 8);
		bytes negative = cat({header(), record(-7, {})});
		reader r6;
		r6.feed(negative.data(), negative.size());
		CHECK(r6.failed());
		// After a failure, more bytes change nothing.
		r6.feed(back.data(), back.size());
		CHECK(r6.failed() && r6.known_frame() == -1);
	}
	// A long game: hundreds of thousands of records stream through without trouble.
	{
		reader r;
		bytes h = header();
		r.feed(h.data(), h.size());
		size_t total = 0;
		for (int f = 0; f != 200000; ++f) {
			bytes rec = f % 3 ? record(f, {}) : cat({record(f, {1, 0x2B, 0}), record(f, {})});
			r.feed(rec.data(), rec.size());
			total += r.take_actions().size();
		}
		CHECK(!r.failed() && r.known_frame() == 199999 && total == (200000 + 2) / 3 * 8);
	}

	// Playing from the queue of arrived frames (ui/live_buffer.h).
	{
		live_buffer::queue q(12);
		// Frames queued up: play on, at the viewer's speed.
		CHECK(!q.hold(0, 300, true) && !q.hold(299, 300, true));
		// Caught up with the newest frame: wait...
		CHECK(q.hold(300, 300, true));
		// ...until a dozen more have arrived, not at the first one, so playback does not stop
		// and start every few frames.
		CHECK(q.hold(300, 301, true) && q.hold(300, 311, true));
		CHECK(!q.hold(300, 312, true) && !q.hold(305, 312, true));
		// The game is over: play out the rest without waiting.
		CHECK(q.hold(312, 312, true) && !q.hold(312, 315, false) && !q.hold(315, 315, false));
		// Playback ahead of the newest frame (it can't be, but the numbers say so): wait.
		live_buffer::queue r;
		CHECK(r.hold(50, 40, true));
		// A nonsense refill falls back to the default.
		live_buffer::queue z(-3);
		CHECK(z.hold(10, 10, true) && z.hold(10, 21, true) && !z.hold(10, 22, true));
	}
	{
		// The shown edge: the newest frame while the game is played...
		live_buffer::edge e;
		CHECK(e.shown(0) == -1);
		for (int k = 0; k <= 20; ++k) e.arrived(k * 0.5, k * 20);  // 40 frames a second for 10 s
		CHECK(e.shown(10) == 400 && e.shown(99) == 400 && !e.ended());
		CHECK(e.rate() > 39 && e.rate() < 41);
		// ... then, once it is over, carrying on at that rate, so it does not look finished.
		e.end(10);
		CHECK(e.ended());
		CHECK(e.shown(10) == 400 && e.shown(11) > 439 && e.shown(11) < 441 && e.shown(70) > 2790);
		e.arrived(12, 900);  // nothing arrives after the end
		e.end(50);           // and the first end counts
		CHECK(e.shown(11) > 439 && e.shown(11) < 441);
		CHECK(e.shown(5) == 400);  // never behind the real end
		// A decoder catching up (thousands of frames at once) is not a rate to keep.
		live_buffer::edge burst;
		burst.arrived(0, 0);
		burst.arrived(1.5, 20000);
		burst.end(1.5);
		CHECK(burst.rate() <= 3000.0 / 42 + 1e-9 && burst.shown(2.5) <= 20000 + 3000.0 / 42 + 1e-6);
		// Ended before any rate is known (opened on a finished game): the slowest rate, 1x.
		live_buffer::edge late;
		late.arrived(0, 5000);
		late.end(0);
		CHECK(late.shown(42) > 5999 && late.shown(42) < 6001);
		// Old samples age out of the rate: a slower recent pace is what counts.
		live_buffer::edge slow;
		for (int k = 0; k <= 10; ++k) slow.arrived(k, k * 70);            // 70 f/s
		for (int k = 11; k <= 30; ++k) slow.arrived(k, 700 + (k - 10) * 30);  // then 30 f/s
		CHECK(slow.rate() > 29 && slow.rate() < 31);
	}
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("live_stream: all passed\n");
	return 0;
}
