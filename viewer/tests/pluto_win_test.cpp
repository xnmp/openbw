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
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
