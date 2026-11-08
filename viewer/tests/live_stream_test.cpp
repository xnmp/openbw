// Behaviour tests for following a live replay (ui/live_stream.h) and pacing it (ui/live_pacer.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/live_stream_test.cpp -o /tmp/live_stream_test && /tmp/live_stream_test
#include "live_stream.h"
#include "live_pacer.h"

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

	using live_pacer::pacer;
	// Frames arriving steadily at 60 a second: playback runs at that rate, a little behind.
	{
		pacer p;
		int cur = 0;
		double t = 0;
		int known = 0;
		for (int i = 0; i != 165 * 10; ++i) {  // ten seconds of 165 Hz display frames
			t += 1.0 / 165;
			while (known < (int)(t * 60)) ++known;
			if (i % 8 == 0) p.known(known, t);  // the decoder writes every ~50 ms
			cur += p.step(cur, t);
			CHECK(cur <= known);
		}
		CHECK(std::fabs(p.rate() - 60) < 3);
		double behind = (known - cur) / 60.0;
		CHECK(behind > 0.1 && behind < 1.0);
		// Smooth: over a second of display frames, no step jumps more than a few game frames.
		int worst = 0;
		for (int i = 0; i != 165; ++i) {
			t += 1.0 / 165;
			while (known < (int)(t * 60)) ++known;
			if (i % 8 == 0) p.known(known, t);
			int s = p.step(cur, t);
			worst = std::max(worst, s);
			cur += s;
		}
		CHECK(worst <= 2);
	}
	// A stall (the game stops for 2 s) holds playback at the newest frame, then it catches up
	// faster than the game runs, without jumping.
	{
		pacer p;
		int cur = 0;
		double t = 0;
		int known = 0;
		auto run = [&](double seconds, bool arriving, int& max_step) {
			for (int i = 0, n = (int)(seconds * 165); i != n; ++i) {
				t += 1.0 / 165;
				if (arriving) known += (i % 11 == 0) ? 4 : 0;  // 60/s in bursts of 4
				p.known(known, t);
				int s = p.step(cur, t);
				max_step = std::max(max_step, s);
				cur += s;
				CHECK(cur <= known);
			}
		};
		int m = 0;
		run(5, true, m);
		run(2, false, m);
		CHECK(cur == known);  // caught up to the stall's edge, and held
		int at_resume = known - cur;
		m = 0;
		run(1, true, m);
		CHECK(m <= 3);
		CHECK(known - cur < at_resume + 60);  // gaining on it
		run(3, true, m);
		CHECK((known - cur) / 60.0 < 1.0);
	}
	// Joining far behind (a pause, or starting late) jumps near the edge instead of crawling.
	{
		pacer p;
		p.known(100, 0);
		p.known(700, 10);  // 60/s
		int s = p.step(0, 10);
		CHECK(s > 600 && s <= 700);
	}
	// Nothing known: nothing to play; nonsense delay falls back to the default.
	{
		pacer p(-1);
		CHECK(p.goal(5) < 0 && p.step(0, 5) == 0);
		pacer q(std::nan(""));
		q.known(10, 1);
		CHECK(q.goal(1) >= 0 && q.goal(1) <= 10);
		// Going back in time or frames repeating don't move anything backwards.
		q.known(5, 2);
		CHECK(q.newest() == 10);
	}

	{
		// At the live edge, give or take: playback a moment behind counts; far behind does not
		// unless the slack (a seek-bar pixel's worth) covers it.
		live_pacer::pacer p;
		CHECK(!p.at_edge(0, 0) && p.quiet_seconds(5) == 0);  // nothing known yet
		for (int i = 0; i <= 40; ++i) p.known(i * 6, i * 0.25);  // 24 frames a second
		double t = 10;
		CHECK(p.at_edge(240, t) && p.at_edge(232, t) && !p.at_edge(180, t));
		CHECK(p.at_edge(180, t, 70) && !p.at_edge(100, t, 70));
		CHECK(p.quiet_seconds(t) == 0 && p.quiet_seconds(t + 4) == 4 && p.quiet_seconds(t - 1) == 0);
	}
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("live_stream: all passed\n");
	return 0;
}
