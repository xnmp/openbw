// Behaviour tests for the seek-bar markers (ui/timeline_markers.h).
// Build and run: cmake --build build --target openbw-timeline-test && ./build/openbw-timeline-test
#include "timeline_markers.h"

#include <cstdio>

using timeline::loss;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const int S = timeline::params{}.bin_frames;  // frames per game second

// `n` units of `value` lost by `owner`, one every `step` seconds from second `at`.
static void add(std::vector<loss>& v, int at, int n, int owner, int value, int step = 1) {
	for (int i = 0; i != n; ++i) v.push_back({(at + i * step) * S, owner, value, 0});
}

int main() {
	{  // nothing lost: no markers
		auto m = timeline::find_markers({}, 10000);
		CHECK(!m.has_first_loss);
		CHECK(m.battles.empty());
	}
	{  // first loss is the earliest, whatever the input order; small losses make no battle
		std::vector<loss> v = {{900, 1, 50, 7}, {300, 0, 75, 41}, {600, 1, 50, 7}};
		auto m = timeline::find_markers(v, 10000);
		CHECK(m.has_first_loss);
		CHECK(m.first_loss.frame == 300);
		CHECK(m.first_loss.owner == 0);
		CHECK(m.first_loss.unit_type == 41);
		CHECK(m.battles.empty());
	}
	{  // one fight: spans its first to last loss, value split by owner
		std::vector<loss> v;
		add(v, 100, 10, 0, 125);  // seconds 100..109
		add(v, 102, 8, 1, 150);   // seconds 102..109
		auto m = timeline::find_markers(v, 200 * S);
		CHECK(m.battles.size() == 1);
		if (m.battles.size() == 1) {
			auto& b = m.battles[0];
			CHECK(b.start == 100 * S);
			CHECK(b.end == 109 * S);
			CHECK(b.lost[0] == 1250);
			CHECK(b.lost[1] == 1200);
			CHECK(b.total == 2450);
		}
	}
	{  // the same value spread thinly over minutes is attrition, not a battle
		std::vector<loss> v;
		add(v, 0, 40, 0, 100, 10);  // 4000 over 400 s
		CHECK(timeline::find_markers(v, 500 * S).battles.empty());
	}
	{  // two separate fights, reported in time order; a fight next to a bigger one merges into it
		std::vector<loss> v;
		add(v, 600, 10, 1, 400);  // big, later
		add(v, 200, 10, 0, 300);  // smaller, earlier
		add(v, 615, 5, 0, 100);   // within the big one's window
		auto m = timeline::find_markers(v, 900 * S);
		CHECK(m.battles.size() == 2);
		if (m.battles.size() == 2) {
			CHECK(m.battles[0].start == 200 * S);
			CHECK(m.battles[1].start == 600 * S);
			CHECK(m.battles[1].lost[0] == 500);
			CHECK(m.battles[1].lost[1] == 4000);
		}
	}
	{  // battles closer than the gap: only the bigger one is kept
		std::vector<loss> v;
		add(v, 100, 10, 0, 300);
		add(v, 135, 10, 1, 500);  // 15 s after the first window ends
		auto m = timeline::find_markers(v, 300 * S);
		CHECK(m.battles.size() == 1);
		if (!m.battles.empty()) CHECK(m.battles[0].lost[1] == 5000);
	}
	{  // capped at max_battles, keeping the biggest
		std::vector<loss> v;
		for (int i = 0; i != 20; ++i) add(v, 100 + i * 100, 10, i % 2, 300 + i * 10);
		timeline::params p;
		p.max_battles = 5;
		auto m = timeline::find_markers(v, 2200 * S, p);
		CHECK(m.battles.size() == 5);
		if (m.battles.size() == 5) CHECK(m.battles[0].start == 1600 * S);  // the five last (largest), in order
	}
	{  // malformed losses are ignored: negative frames, past the end, bad owners, negative value
		std::vector<loss> v = {{-24, 0, 5000, 0}, {100000, 0, 5000, 0}, {240, 9, 5000, 0}, {240, -1, 5000, 0}, {240, 0, -5000, 0}};
		auto m = timeline::find_markers(v, 2400);
		CHECK(!m.has_first_loss);
		CHECK(m.battles.empty());
	}
	{  // a loss on the very last frame and a zero-length replay don't overrun
		std::vector<loss> v;
		add(v, 0, 1, 0, 3000);
		auto m = timeline::find_markers(v, 0);
		CHECK(m.battles.size() == 1);
		std::vector<loss> w = {{2400, 1, 3000, 0}};
		CHECK(timeline::find_markers(w, 2400).battles.size() == 1);
	}
	{  // very large values don't overflow the window sums
		std::vector<loss> v;
		add(v, 10, 10, 0, 1 << 28);
		auto m = timeline::find_markers(v, 100 * S);
		CHECK(m.battles.size() == 1);
	}
	{  // a fight longer than the window is reported whole, while losses keep coming
		std::vector<loss> v;
		add(v, 100, 30, 0, 200, 2);  // a loss every 2 s for 60 s
		add(v, 170, 1, 1, 100);      // 12 s after: past the 5 s gap, not part of it
		auto m = timeline::find_markers(v, 400 * S);
		CHECK(m.battles.size() == 1);
		if (m.battles.size() == 1) {
			CHECK(m.battles[0].start == 100 * S);
			CHECK(m.battles[0].end == 158 * S);
			CHECK(m.battles[0].total == 6000);
		}
	}
	{  // extension stops halfway to the next battle
		std::vector<loss> v;
		add(v, 100, 10, 0, 400);
		add(v, 110, 20, 0, 10, 2);   // trickle on to 148 s
		add(v, 160, 10, 1, 400);
		add(v, 110, 0, 0, 0);
		auto m = timeline::find_markers(v, 400 * S);
		CHECK(m.battles.size() == 2);
		if (m.battles.size() == 2) CHECK(m.battles[0].end < m.battles[1].start);
	}
	{  // two equal fights too close together: one battle, the earlier (ties pick the earliest
	   // window). Windows slide, so lone losses more than gap_bins apart are separate battles.
		timeline::params p;
		p.extend_gap_bins = 0;
		auto two = [&](int apart) {
			std::vector<loss> v;
			add(v, 100, 1, 0, 3000);
			add(v, 100 + apart, 1, 1, 3000);
			return timeline::find_markers(v, 400 * S, p);
		};
		auto m = two(20);
		CHECK(m.battles.size() == 1);
		if (!m.battles.empty()) CHECK(m.battles[0].start == 100 * S);
		CHECK(two(p.gap_bins).battles.size() == 1);
		CHECK(two(p.gap_bins + 1).battles.size() == 2);
	}
	{  // marker_at: first loss wins over a battle under it; tolerance and lead
		std::vector<loss> v;
		add(v, 100, 10, 0, 300);
		auto m = timeline::find_markers(v, 400 * S);
		auto h = timeline::marker_at(m, 100 * S + 5, 10, 5 * S);
		CHECK(h.kind == timeline::hit::first_loss);
		CHECK(h.seek_frame == 95 * S);
		h = timeline::marker_at(m, 105 * S, 10, 5 * S);
		CHECK(h.kind == timeline::hit::battle);
		CHECK(h.battle_index == 0);
		CHECK(h.seek_frame == 95 * S);
		CHECK(timeline::marker_at(m, 109 * S + 10, 10, 0).kind == timeline::hit::battle);
		CHECK(timeline::marker_at(m, 109 * S + 11, 10, 0).kind == timeline::hit::none);
		CHECK(timeline::marker_at(m, 0, 10, 5 * S).kind == timeline::hit::none);
		auto early = timeline::find_markers({{24, 0, 50, 0}}, 400 * S);
		CHECK(timeline::marker_at(early, 24, 0, 5 * S).seek_frame == 0);  // not before the start
		CHECK(timeline::marker_at(timeline::markers{}, 24, 1000, 0).kind == timeline::hit::none);
	}
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
