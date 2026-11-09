#ifndef OPENBW_LIVE_BUFFER_H
#define OPENBW_LIVE_BUFFER_H

// Playing a live game from a queue: frames arrive unevenly (self-play games are not held to
// real time; they run as fast as the bots manage, at half to twice their average from one
// second to the next), so the viewer does not chase them. It plays at its own speed (1x
// unless changed) through the frames that have arrived. Self-play usually runs faster than
// 1x, so the queue grows; the speed keys catch up. Only when playback reaches the newest
// frame does it wait, and then until `refill` frames have queued up again, so a game that
// arrives at about the playing speed does not stop and start every few frames. Pure, so it
// can be tested.

#include <algorithm>
#include <deque>
#include <utility>

namespace live_buffer {

class queue {
public:
	explicit queue(int refill_frames = 12) : refill(refill_frames > 0 ? refill_frames : 12) {}

	// Whether to hold playback at `current`, with frames up to `newest` arrived and `more`
	// whether any are still to come. Call once per shown frame.
	bool hold(int current, int newest, bool more) {
		if (!more) return waiting = false;  // the game is over: what is there is all there is
		int queued = newest - current;
		if (queued <= 0) waiting = true;
		else if (waiting && queued >= refill) waiting = false;
		return waiting;
	}
	bool waiting_for_more() const { return waiting; }

private:
	int refill;
	bool waiting = false;
};

// The live edge as shown, so that a finished game looks like it is still being played
// until playback reaches its end (no spoilers: the clock, the "behind" time and the seek bar
// would otherwise stop growing the moment it ended). While the game is played the edge is the
// newest frame; once it is over it carries on at the rate frames were arriving (over the last
// `window` seconds, within `min_rate`..`max_rate` frames a second: not the burst of a decoder
// catching up), without bound. Playback itself still stops at the real end.
class edge {
public:
	explicit edge(double window = 10, double min_rate = 1000.0 / 42, double max_rate = 3000.0 / 42)
		: window(window), min_rate(min_rate), max_rate(max_rate) {}

	// Frames up to `frame` had arrived by time `t` (seconds).
	void arrived(double t, int frame) {
		if (over || frame <= newest) return;
		newest = frame;
		samples.emplace_back(t, frame);
		while (samples.size() > 2 && samples.front().first < t - window) samples.pop_front();
	}
	// The game ended at time `t` (at the newest frame). Repeated calls keep the first.
	void end(double t) {
		if (over) return;
		over = true;
		end_t = t;
		end_rate = rate();
	}
	bool ended() const { return over; }
	// Frames a second arriving lately (the slowest when not yet known).
	double rate() const {
		if (samples.size() < 2) return min_rate;
		double dt = samples.back().first - samples.front().first;
		if (dt < 1) return min_rate;
		return std::max(min_rate, std::min(max_rate, (samples.back().second - samples.front().second) / dt));
	}
	// The edge to show at time `t`.
	double shown(double t) const {
		if (!over) return newest;
		return newest + end_rate * std::max(0.0, t - end_t);
	}

private:
	double window, min_rate, max_rate;
	std::deque<std::pair<double, int>> samples;
	int newest = -1;
	bool over = false;
	double end_t = 0, end_rate = 0;
};

}  // namespace live_buffer

#endif
