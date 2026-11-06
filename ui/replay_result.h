#ifndef OPENBW_UI_REPLAY_RESULT_H
#define OPENBW_UI_REPLAY_RESULT_H

// The result shown when a replay ends, from each player's victory state as the game keeps
// it (player_t::victory_state: 0 still playing, 1 dropped, 2 defeated or left, 3 and up
// victorious). A replay usually ends when the loser leaves, before the game declares the
// survivor victorious, so a single player left standing among defeated ones is the winner;
// once a winner is declared, the others in the game have lost.
// Pure: no game state, so it can be tested on its own (viewer/tests/replay_result_test.cpp).

#include <array>
#include <vector>

namespace replay_result {

enum class outcome { absent, playing, victorious, defeated, dropped };

struct result {
	std::array<outcome, 8> players{};  // absent for slots not in the game
	std::vector<int> winners;          // slots, in order; empty if undecided
};

// `in_game[i]`: slot i was in the game at its start; `state[i]`: its victory state now.
inline result decide(const std::array<bool, 8>& in_game, const std::array<int, 8>& state) {
	result r;
	std::vector<int> playing;
	for (int i = 0; i != 8; ++i) {
		if (!in_game[i]) continue;
		int s = state[i];
		outcome o = s >= 3 ? outcome::victorious : s == 2 ? outcome::defeated : s == 1 ? outcome::dropped : s == 0 ? outcome::playing : outcome::defeated;
		r.players[i] = o;
		if (o == outcome::victorious) r.winners.push_back(i);
		if (o == outcome::playing) playing.push_back(i);
	}
	// The game declared a winner: everyone else in the game lost, even if the replay ends
	// before their defeat is recorded.
	if (!r.winners.empty()) {
		for (int i = 0; i != 8; ++i) if (r.players[i] == outcome::playing) r.players[i] = outcome::defeated;
		return r;
	}
	bool anyone_out = false;
	for (int i = 0; i != 8; ++i) anyone_out |= r.players[i] == outcome::defeated || r.players[i] == outcome::dropped;
	if (r.winners.empty() && playing.size() == 1 && anyone_out) {
		r.winners.push_back(playing[0]);
		r.players[playing[0]] = outcome::victorious;
	}
	return r;
}

inline const char* label(outcome o) {
	switch (o) {
	case outcome::victorious: return "Victorious";
	case outcome::defeated: return "Defeated";
	case outcome::dropped: return "Dropped";
	case outcome::playing: return "Still playing";
	default: return "";
	}
}

}

#endif
