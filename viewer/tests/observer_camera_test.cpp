// Behaviour tests for the auto observer's camera and shot choice (ui/observer_camera.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/observer_camera_test.cpp -o /tmp/observer_camera_test && /tmp/observer_camera_test
#include "observer_camera.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace observer_camera;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const double VW = 1280, VH = 720, DT = 1.0 / 60;

// A small deterministic noise source.
static double noise(unsigned& s) { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xffff) / 65535.0 - 0.5; }

int main() {
	// A scene one view away is framed within a second and a half, and the camera never
	// overshoots it on the way.
	{
		rig r; r.place(1000, 1000);
		double tx = 1000 + VW, ty = 1000, prev = r.x();
		bool overshoot = false;
		for (int i = 0; i < 90; ++i) {
			CHECK(!r.step(tx, ty, DT, VW, VH, 1));
			if (r.x() > tx + 1e-6) overshoot = true;
			CHECK(r.x() >= prev - 1e-9);  // never backs up
			prev = r.x();
		}
		CHECK(!overshoot);
		CHECK(std::fabs(r.x() - tx) < VW * 0.03 && std::fabs(r.y() - ty) < 1e-6);
	}

	// The picture is continuous: a new target never moves the camera more than top speed
	// allows in a frame, and the first frames after the target changes barely accelerate it
	// (no snap at the start of a pan).
	{
		rig r; r.place(1000, 1000);
		double max_step = 0, v_after_first = 0;
		for (int i = 0; i < 240; ++i) {
			double tx = i < 120 ? 1000 : 1000 + 1.5 * VW, ty = 1000 + (i < 120 ? 0 : 300);
			double x0 = r.x(), y0 = r.y();
			r.step(tx, ty, DT, VW, VH, 1);
			max_step = std::max(max_step, std::hypot(r.x() - x0, r.y() - y0));
			if (i == 120) v_after_first = std::hypot(r.cam.vx, r.cam.vy);
		}
		CHECK(max_step <= 3.0 * VW * DT * 1.001);
		CHECK(v_after_first < 0.05 * VW);  // < 5% of a view per second one frame into the pan
	}

	// A scene a few views away is cut to; nearer ones are panned.
	{
		rig r; r.place(1000, 1000);
		CHECK(r.step(1000 + 6 * VW, 1000, DT, VW, VH, 1));
		CHECK(std::fabs(r.x() - (1000 + 6 * VW)) < 1e-9);
		rig q; q.place(1000, 1000);
		CHECK(!q.step(1000 + 2 * VW, 1000, DT, VW, VH, 1));
		CHECK(q.x() < 1000 + 0.5 * VW);
	}

	// Jitter in the scene's centre (a few percent of a view) is not passed on: the camera
	// moves far less than the target does.
	{
		rig r; r.place(2000, 2000);
		unsigned s = 7;
		double target_travel = 0, cam_travel = 0, px = 2000, py = 2000, cx = r.x(), cy = r.y();
		for (int i = 0; i < 600; ++i) {
			double tx = 2000 + noise(s) * 0.06 * VW, ty = 2000 + noise(s) * 0.06 * VH;
			target_travel += std::hypot(tx - px, ty - py); px = tx; py = ty;
			r.step(tx, ty, DT, VW, VH, 1);
			cam_travel += std::hypot(r.x() - cx, r.y() - cy); cx = r.x(); cy = r.y();
		}
		CHECK(cam_travel < 0.25 * target_travel);
	}

	// ... but a slowly drifting scene is followed continuously (moving every frame), not in
	// bursts: no stretch of a second without motion while the target keeps going.
	{
		rig r; r.place(2000, 2000);
		double x0 = r.x();
		int still_run = 0, worst = 0;
		for (int i = 0; i < 1200; ++i) {
			double before = r.x();
			r.step(2000 + i * 0.5, 2000, DT, VW, VH, 1);  // 30 px/s: ~2% of a view per second
			if (i > 120) { still_run = std::fabs(r.x() - before) < 1e-3 ? still_run + 1 : 0; worst = std::max(worst, still_run); }
		}
		CHECK(r.x() - x0 > 500);
		CHECK(worst < 60);
	}

	// Degenerate input: no time, a huge step, paused playback or NaN-free extremes leave a
	// finite, sane camera.
	{
		rig r; r.place(500, 500);
		r.step(900, 900, 0, VW, VH, 1);
		CHECK(r.x() == 500 && r.y() == 500);
		r.step(900, 900, 1e9, VW, VH, 1);
		CHECK(std::isfinite(r.x()) && std::isfinite(r.y()) && r.x() < 900.001 && r.x() >= 500);
		r.step(900, 900, DT, VW, VH, 0);
		CHECK(std::isfinite(r.x()));
		r.step(900, 900, DT, VW, VH, 1e9);
		CHECK(std::isfinite(r.x()) && std::isfinite(r.cam.vx));
		r.step(900, 900, -1, VW, VH, 1);
		CHECK(std::isfinite(r.x()));
	}

	// Faster playback catches up sooner.
	{
		rig a, b; a.place(0, 0); b.place(0, 0);
		for (int i = 0; i < 30; ++i) { a.step(VW, 0, DT, VW, VH, 1); b.step(VW, 0, DT, VW, VH, 8); }
		CHECK(b.x() > a.x());
	}

	// ---- Shot choice ----
	const double TICK = 4 * 0.042;  // an observer tick at normal speed
	auto run_until_switch = [&](shot_director& d, scene cur, scene best, double t0, double limit) {
		for (double t = t0; t < t0 + limit; t += TICK) if (d.tick(t, cur, best, VW, VH)) return t - t0;
		return -1.0;
	};

	// A fight starting where nothing happens is on screen quickly: within a second of its
	// first showing up.
	{
		shot_director d; d.start_shot(0, 0);
		double dt = run_until_switch(d, {0.5, 0, 500, 500}, {40, 40, 3000, 1000}, 10, 5);
		CHECK(dt >= 0 && dt <= 1.0);
	}
	// A fight much bigger than the one shown takes over faster than one only a little bigger.
	{
		shot_director a, b; a.start_shot(9.8, 20); b.start_shot(9.8, 20);
		double big = run_until_switch(a, {20, 20, 500, 500}, {200, 150, 3000, 1000}, 10, 8);
		double mid = run_until_switch(b, {20, 20, 500, 500}, {30, 25, 3000, 1000}, 10, 8);
		CHECK(big >= 0 && mid >= 0 && big < mid);
		CHECK(big <= 0.7);
	}
	// A challenger that is not clearly better never wins, and neither does one already in view.
	{
		shot_director d; d.start_shot(0, 20);
		CHECK(run_until_switch(d, {20, 20, 500, 500}, {24, 22, 3000, 1000}, 10, 60) < 0);
		shot_director e; e.start_shot(0, 20);
		CHECK(run_until_switch(e, {20, 20, 500, 500}, {80, 60, 500 + VW * 0.3, 500}, 10, 60) < 0);
	}
	// Two equal fights with fluctuating scores: the shot never flips back and forth.
	{
		shot_director d; d.start_shot(0, 30);
		unsigned s = 3;
		scene shown{30, 30, 500, 500}, other{30, 30, 3500, 500};
		int switches = 0;
		for (double t = 10; t < 130; t += TICK) {
			shown.score = shown.action = 30 * (1 + 0.3 * noise(s));
			other.score = other.action = 30 * (1 + 0.3 * noise(s));
			if (d.tick(t, shown, other, VW, VH)) { ++switches; d.start_shot(t, other.action); std::swap(shown, other); }
		}
		CHECK(switches <= 1);
	}
	// Moving from one quiet place to another waits, so a tour does not flicker.
	{
		shot_director d; d.start_shot(0, 0);
		double dt = run_until_switch(d, {1, 0, 500, 500}, {3, 0.5, 3000, 1000}, 0.5, 20);
		CHECK(dt >= 3.0);
	}
	// A shot that has just started is not abandoned for a merely bigger fight at once.
	{
		shot_director d; d.start_shot(10, 20);
		CHECK(!d.tick(10.2, {20, 20, 500, 500}, {40, 35, 3000, 1000}, VW, VH));
		CHECK(!d.tick(10.4, {20, 20, 500, 500}, {40, 35, 3000, 1000}, VW, VH));
	}
	// Faster playback shortens the holds.
	{
		shot_director a, b; a.start_shot(0, 20); b.start_shot(0, 20);
		auto tt = [&](shot_director& d, double speed) {
			for (double t = 10; t < 20; t += TICK) if (d.tick(t, {20, 20, 500, 500}, {40, 35, 3000, 1000}, VW, VH, speed)) return t - 10;
			return -1.0;
		};
		double slow = tt(a, 1), fast = tt(b, 8);
		CHECK(slow >= 0 && fast >= 0 && fast <= slow);
	}
	// Non-finite scores do not switch the shot.
	{
		shot_director d; d.start_shot(0, 20);
		double nan = std::numeric_limits<double>::quiet_NaN();
		CHECK(run_until_switch(d, {20, 20, 500, 500}, {nan, nan, 3000, 1000}, 10, 20) < 0);
	}

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("observer_camera_test: ok\n");
	return 0;
}
