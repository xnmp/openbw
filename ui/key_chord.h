#ifndef OPENBW_KEY_CHORD_H
#define OPENBW_KEY_CHORD_H

// Two-key chords: a prefix (a key with Alt held, e.g. Alt+M), then a plain key within a
// couple of seconds (B). The prefix and the key after it are the chord's, so neither does
// what it would alone (M mutes, B toggles health bars). Modifier keys pressed in between are
// ignored; any other key, or waiting too long, ends the chord. Pure (the caller passes the
// time, in seconds), so it can be tested.

#include <cctype>

namespace key_chord {

enum class result {
	none,       // not the chord's: handle the key as usual
	started,    // the prefix: wait for the second key
	completed,  // the second key of a chord (second_key() says which)
	cancelled,  // a key that ends the chord without completing it (swallowed)
};

class reader {
public:
	reader(int prefix_key, double timeout_seconds = 2.0) : prefix(prefix_key), timeout(timeout_seconds) {}

	// A key press: `sym` as the window reports it, `alt` whether Alt is held.
	result key(int sym, bool alt, double t) {
		if (is_modifier(sym)) return pending(t) ? result::started : result::none;
		if (pending(t)) {
			waiting = false;
			if (sym == 27) return result::cancelled;
			second = lower(sym);
			return result::completed;
		}
		waiting = false;
		if (alt && lower(sym) == prefix) {
			waiting = true;
			since = t;
			return result::started;
		}
		return result::none;
	}

	bool pending(double t) const { return waiting && t - since <= timeout && t >= since; }
	int second_key() const { return second; }

private:
	int prefix;
	double timeout;
	bool waiting = false;
	double since = 0;
	int second = -1;
	static int lower(int sym) { return sym >= 'A' && sym <= 'Z' ? sym - 'A' + 'a' : sym; }
	// SDL keycodes of Ctrl, Shift, Alt and GUI (left and right), and Caps Lock.
	static bool is_modifier(int sym) { return (sym >= 0x400000E0 && sym <= 0x400000E7) || sym == 0x40000039; }
};

}  // namespace key_chord

#endif
