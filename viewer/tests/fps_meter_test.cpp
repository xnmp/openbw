// Behaviour tests for the FPS readout's meter (ui/fps_meter.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/fps_meter_test.cpp -o /tmp/fps_meter_test && /tmp/fps_meter_test
#include "fps_meter.h"

#include <cmath>
#include <cstdio>

using fps_meter::meter;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double tol = 0.5) { return std::fabs(a - b) <= tol; }

static void run(meter& m, double& t, double hz, double seconds) {
	for (int i = 0, n = (int)std::lround(hz * seconds); i != n; ++i) m.tick(t += 1.0 / hz);
}

int main() {
	// Nothing to report before a full period.
	meter m;
	CHECK(!m.ready());
	double t = 100;
	m.tick(t);
	run(m, t, 60, 0.4);
	CHECK(!m.ready());
	// A steady 60, 165 or 240 frames a second reads as that.
	run(m, t, 60, 1.0);
	CHECK(m.ready() && near(m.fps(), 60));
	run(m, t, 165, 1.0);
	CHECK(near(m.fps(), 165, 1));
	run(m, t, 240, 1.0);
	CHECK(near(m.fps(), 240, 1));
	// It follows a drop within a period or so.
	run(m, t, 30, 1.0);
	CHECK(near(m.fps(), 30));
	// A stall (one frame after a long pause) reads low, not as an average of better times.
	m.tick(t += 2.0);
	CHECK(m.fps() > 0 && m.fps() < 10);  // the frames of the part-period before it count too

	// A clock that goes back starts over rather than reporting nonsense; the last value stays.
	meter b;
	double u = 50;
	b.tick(u);
	run(b, u, 60, 1.0);
	double before = b.fps();
	b.tick(u - 10);
	CHECK(b.fps() == before);
	u -= 10;
	run(b, u, 120, 1.0);
	CHECK(near(b.fps(), 120, 1));

	// A nonsensical period falls back to the default instead of reporting every frame.
	meter c(0), d(-1);
	double v = 0;
	c.tick(v);
	d.tick(v);
	c.tick(v + 0.1);
	d.tick(v + 0.1);
	CHECK(!c.ready() && !d.ready());

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("fps_meter: all passed\n");
	return 0;
}
