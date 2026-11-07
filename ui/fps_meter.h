#ifndef OPENBW_FPS_METER_H
#define OPENBW_FPS_METER_H

// The viewer's frame rate, for the FPS readout: frames counted over a short period and
// reported as one number per period, so the readout is steady enough to read. Pure (the
// caller passes the time), so it can be tested.

namespace fps_meter {

class meter {
public:
	explicit meter(double period_seconds = 0.5) : period(period_seconds > 0 ? period_seconds : 0.5) {}

	// One shown frame at time `t` (seconds, monotonic).
	void tick(double t) {
		if (!started || t < start) {  // the first frame, or a clock that went back: start over
			started = true;
			start = t;
			frames = 0;
			return;
		}
		++frames;
		double elapsed = t - start;
		if (elapsed >= period) {
			value = frames / elapsed;
			has_value = true;
			start = t;
			frames = 0;
		}
	}

	// Frames per second over the last full period; false until one has passed.
	bool ready() const { return has_value; }
	double fps() const { return value; }

private:
	double period;
	bool started = false, has_value = false;
	double start = 0, value = 0;
	long frames = 0;
};

}  // namespace fps_meter

#endif
