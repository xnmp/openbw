// Behaviour tests for reading the Pluto bot's win estimate (ui/pluto_win.h).
// Build and run: cmake --build build --target openbw-pluto-win-test && ./build/openbw-pluto-win-test
#include "pluto_win.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
	const std::string file =
		"seq 4\n"
		"12Nexus 3 1\n"
		"end\n"
		"{\"t\":\"start\",\"id\":1,\"opp\":\"PlutoZ\",\"map\":\"x\"}\n"
		"{\"t\":\"end\",\"id\":1,\"opp\":\"PlutoZ\",\"result\":\"win\",\"frames\":6400,\"slip\":[1,0],\"win_final\":0.9,\"win_traj\":[0.2,-0.4,1.0,0.6]}\n"
		"{\"t\":\"event\",\"id\":1,\"what\":\"degraded\",\"detail\":\"a, b\"}\n"
		"{\"t\":\"end\",\"id\":2,\"opp\":\"PlutoZ\",\"result\":\"loss\",\"frames\":20000,\"win_traj\":[0.1,0.2]}\n"
		"{\"t\":\"end\",\"id\":3,\"opp\":\"Other\",\"frames\":6400,\"win_traj\":[0.5]}\n";
	auto recs = pluto_win::parse(file);
	CHECK(recs.size() == 3);
	if (recs.size() == 3) {
		CHECK(recs[0].opponent == "PlutoZ");
		CHECK(recs[0].frames == 6400);
		CHECK(recs[0].traj.size() == 4);
		CHECK(near(recs[0].traj[1], -0.4));
	}
	{  // matched by opponent and length; the closest game in time order wins ties (last)
		CHECK(pluto_win::match(recs, "PlutoZ", 6410) == &recs[0]);
		CHECK(pluto_win::match(recs, "PlutoZ", 20100) == &recs[1]);
		CHECK(pluto_win::match(recs, "PlutoZ", 12000) == nullptr);  // no game that long
		CHECK(pluto_win::match(recs, "Nobody", 6400) == nullptr);
		CHECK(pluto_win::match(recs, "Other", 6400) == &recs[2]);
	}
	if (!recs.empty()) {  // chance_at: points at segment middles (800, 2400, 4000, 5600 here)
		auto& r = recs[0];
		CHECK(near(pluto_win::chance_at(r, 0), 0.6));      // flat before the first point
		CHECK(near(pluto_win::chance_at(r, 800), 0.6));
		CHECK(near(pluto_win::chance_at(r, 1600), 0.45));  // halfway between 0.2 and -0.4
		CHECK(near(pluto_win::chance_at(r, 4000), 1.0));
		CHECK(near(pluto_win::chance_at(r, 6400), 0.8));   // flat after the last
		CHECK(near(pluto_win::chance_at(r, 99999), 0.8));
		CHECK(near(pluto_win::chance_at(r, -5), 0.6));
	}
	{  // malformed lines are skipped, not misread
		const std::string bad =
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100}\n"                          // no trajectory
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":0,\"win_traj\":[0.1]}\n"         // no length
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100,\"win_traj\":[0.1,x]}\n"     // bad number
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100,\"win_traj\":[]}\n"          // empty
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100,\"win_traj\":[0.1,0.2\n"     // truncated
			"{\"t\":\"end\",\"frames\":100,\"win_traj\":[0.1]}\n"                     // no opponent
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100,\"win_traj\":[nan,0.1]}\n"   // not finite
			"{\"t\":\"end\",\"opp\":\"A\",\"frames\":100,\"win_traj\":[3,-7]";        // out of range, no newline: clamped
		auto r = pluto_win::parse(bad);
		CHECK(r.size() == 1);
		if (r.size() == 1) {
			CHECK(near(r[0].traj[0], 1.0));
			CHECK(near(r[0].traj[1], -1.0));
		}
		CHECK(pluto_win::parse("").empty());
		CHECK(pluto_win::parse(std::string("\0\xff\n{", 4)).empty());
	}
	{  // empty record gives an even chance rather than crashing
		pluto_win::record r;
		CHECK(near(pluto_win::chance_at(r, 100), 0.5));
	}
	{  // decided(): from when the winner's chance stays high to the end
		auto rec = [](std::vector<double> traj, int frames) { pluto_win::record r; r.opponent = "x"; r.frames = frames; r.traj = traj; return r; };
		// 10 points over 24000 frames (2400 each). A turtles from point 4 on: A loses.
		auto a = rec({0, 0, 0, -0.2, -0.85, -0.9, -0.9, -0.95, -0.95, -1}, 24000);
		auto b = rec({0, 0, 0, 0.2, 0.85, 0.9, 0.9, 0.95, 0.95, 1}, 24000);
		auto d = pluto_win::decided(&a, &b, 24000);
		CHECK(d.winner == 1);
		CHECK(d.frame > 3 * 2400 && d.frame <= 4 * 2400 + 1200);  // between points 3 and 4
		// One estimate alone also works; neither: nothing
		CHECK(pluto_win::decided(&a, nullptr, 24000).winner == 1);
		CHECK(pluto_win::decided(nullptr, &b, 24000).winner == 1);
		CHECK(pluto_win::decided(nullptr, nullptr, 24000).frame == -1);
		// The views are averaged: a winner that is unsure holds it back
		auto unsure = rec({0, 0, 0, 0.2, 0.3, 0.3, 0.4, 0.5, 0.6, 1}, 24000);
		auto d2 = pluto_win::decided(&a, &unsure, 24000);
		CHECK(d2.frame == -1 || d2.frame > d.frame);
		// A comeback: the side behind recovers, so it's decided only at the very end: none
		auto back = rec({0, -0.9, -0.9, -0.9, -0.9, 0, 0.5, 0.9, 0.95, 1}, 24000);
		auto ahead = rec({0, 0.9, 0.9, 0.9, 0.9, 0, -0.5, -0.9, -0.95, -1}, 24000);
		auto d3 = pluto_win::decided(&back, &ahead, 24000);
		CHECK(d3.winner == 0);
		CHECK(d3.frame >= 6 * 2400);  // not during the early lead that didn't last
		// Decided only in the last minute (the resignation itself): none
		std::vector<double> late(64, 0.0);  // real records have 64 points
		late.back() = -1;
		auto late_a = rec(late, 24000);
		CHECK(pluto_win::decided(&late_a, nullptr, 24000).frame == -1);
		// Even all the way (no winner at the end) or nonsense lengths: none
		auto even = rec({0, 0, 0}, 24000);
		CHECK(pluto_win::decided(&even, nullptr, 24000).frame == -1);
		CHECK(pluto_win::decided(&a, &b, 0).frame == -1);
		// Decided from the start: frame 0
		auto lost = rec({-1, -1, -1}, 24000);
		auto d4 = pluto_win::decided(&lost, nullptr, 24000);
		CHECK(d4.frame == 0 && d4.winner == 1);
	}
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
