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

}  // namespace live_buffer

#endif
