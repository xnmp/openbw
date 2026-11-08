#ifndef OPENBW_LIVE_PACER_H
#define OPENBW_LIVE_PACER_H

// How fast to play a live game: as fast as it arrives, a little behind its newest frame.
//
// The newest known frame advances in bursts (the decoder writes a few frames at a time, and a
// player that is thinking can stall the game for a moment). Playing each burst as it lands
// would stutter, so the pacer estimates the rate frames arrive at, extrapolates where the
// game is now, and aims a short delay (`lag`) behind that, never past what is known. When
// playback is behind its aim (after a stall, a pause, or joining late) it catches up at up to
// twice the arrival rate, and jumps if it is very far behind. Pure (the caller passes the
// time, in seconds), so it can be tested.

#include <algorithm>
#include <cmath>

namespace live_pacer {

class pacer {
public:
	explicit pacer(double lag_seconds = 0.35) : lag(lag_seconds > 0 && std::isfinite(lag_seconds) ? lag_seconds : 0.35) {}

	// The newest known frame is `frame`, seen at time `t`.
	void known(int frame, double t) {
		if (frame <= edge) return;
		if (edge < 0) {
			sample_frame = frame, sample_t = t;
		} else if (t - sample_t >= 0.25) {
			double inst = (frame - sample_frame) / (t - sample_t);
			if (std::isfinite(inst) && inst > 0) rate_ = have_rate ? 0.7 * rate_ + 0.3 * inst : inst;
			have_rate = true;
			sample_frame = frame, sample_t = t;
		}
		edge = frame;
		edge_t = t;
	}

	// The frame to aim for at time `t`: the extrapolated newest frame less the delay, never
	// beyond the newest known frame (-1 before anything is known).
	double goal(double t) const {
		if (edge < 0) return -1;
		double now = edge + rate_ * std::max(0.0, t - edge_t);
		return std::max(0.0, std::min((double)edge, now - rate_ * lag));
	}

	// How many frames to advance now, with playback at `current` (call once per shown frame).
	int step(int current, double t) {
		double dt = last_t < 0 ? 0 : std::max(0.0, std::min(0.25, t - last_t));
		last_t = t;
		double g = goal(t);
		if (g < 0 || current >= g) {
			pos = current;
			return 0;
		}
		if (pos < current || pos > g) pos = current;
		if (g - current > rate_ * far_seconds) {  // far behind (joined late): jump
			pos = g;
		} else {
			pos = std::min(g, pos + rate_ * dt * catch_up);
		}
		return std::max(0, (int)std::floor(pos) - current);
	}

	// Frames per second the game arrives at (a default until measured).
	double rate() const { return rate_; }
	int newest() const { return edge; }
	// Frames within which playback counts as at the live edge.
	double near_frames() const { return rate_ * (lag + 0.5); }
	// Whether playback at `frame` is at the live edge at time `t`, give or take `slack` frames
	// (e.g. what one pixel of a seek bar spans).
	bool at_edge(int frame, double t, double slack = 0) const {
		double g = goal(t);
		return g >= 0 && frame >= g - std::max(near_frames(), slack);
	}
	// Seconds since the newest known frame last moved on (0 before anything is known).
	double quiet_seconds(double t) const { return edge < 0 ? 0 : std::max(0.0, t - edge_t); }

private:
	double lag;
	static constexpr double catch_up = 2.0, far_seconds = 10.0;
	int edge = -1;
	double edge_t = 0, rate_ = 24, sample_t = 0, last_t = -1, pos = 0;
	int sample_frame = 0;
	bool have_rate = false;
};

}  // namespace live_pacer

#endif
