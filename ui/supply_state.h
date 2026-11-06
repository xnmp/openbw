#ifndef OPENBW_UI_SUPPLY_STATE_H
#define OPENBW_UI_SUPPLY_STATE_H

// How close a player is to their supply cap, for colouring the HUD's supply count. The game
// counts supply in halves (a Zergling is 1, a Probe 2, a Dragoon 4), so these take the raw
// half-unit values. Blocked: under one supply free, so not even a worker can start (the HUD
// rounds 47.5 up to 48/48). Nearly blocked: under two free, so only a one-supply unit fits
// (47/48: no Dragoon, Marine pair or Hydralisk). At the 200 cap, more supply can't be built,
// so neither applies.
// Pure: no game state, so it can be tested on its own (viewer/tests/supply_state_test.cpp).

namespace supply_state {

enum class pressure { free, nearly_blocked, blocked };

inline pressure of(int used_halves, int available_halves) {
	if (available_halves >= 400) return pressure::free;  // maxed: nothing to build
	int free_halves = available_halves - used_halves;
	if (free_halves < 2) return pressure::blocked;
	if (free_halves < 4) return pressure::nearly_blocked;
	return pressure::free;
}

}

#endif
