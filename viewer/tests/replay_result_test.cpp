// Behaviour tests for the end-of-replay result (ui/replay_result.h).
// Build and run: cmake --build build --target openbw-replay-result-test && ./build/openbw-replay-result-test
#include "replay_result.h"

#include <cstdio>
#include <initializer_list>

using replay_result::outcome;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::array<bool, 8> slots(std::initializer_list<int> s) {
	std::array<bool, 8> r{};
	for (int i : s) r[i] = true;
	return r;
}

int main() {
	{  // the loser left: the one still playing is the winner
		auto r = replay_result::decide(slots({0, 1}), {2, 0});
		CHECK(r.winners == std::vector<int>{1});
		CHECK(r.players[0] == outcome::defeated);
		CHECK(r.players[1] == outcome::victorious);
		CHECK(r.players[2] == outcome::absent);
	}
	{  // the game declared it: victorious as recorded
		auto r = replay_result::decide(slots({0, 1}), {3, 2});
		CHECK(r.winners == std::vector<int>{0});
	}
	{  // declared before the loser's defeat was recorded: the loser is still defeated
		auto r = replay_result::decide(slots({0, 1}), {3, 0});
		CHECK(r.winners == std::vector<int>{0});
		CHECK(r.players[1] == outcome::defeated);
	}
	{  // a dropped player also loses
		auto r = replay_result::decide(slots({0, 1}), {0, 1});
		CHECK(r.winners == std::vector<int>{0});
		CHECK(r.players[1] == outcome::dropped);
	}
	{  // both still in (a frame cap, or the recording stopped): undecided
		auto r = replay_result::decide(slots({0, 1}), {0, 0});
		CHECK(r.winners.empty());
		CHECK(r.players[0] == outcome::playing && r.players[1] == outcome::playing);
	}
	{  // free for all: two still playing after one left is undecided; one left standing wins
		CHECK(replay_result::decide(slots({0, 1, 2}), {2, 0, 0}).winners.empty());
		CHECK(replay_result::decide(slots({0, 1, 2}), {2, 0, 2}).winners == std::vector<int>{1});
	}
	{  // empty slots are ignored, whatever their state
		auto r = replay_result::decide(slots({1, 3}), {5, 2, 7, 0, 0, 0, 0, 0});
		CHECK(r.winners == std::vector<int>{3});
		CHECK(r.players[0] == outcome::absent);
	}
	{  // everyone out (both left on the same frame): no winner
		CHECK(replay_result::decide(slots({0, 1}), {2, 2}).winners.empty());
	}
	{  // a draw declared by the game (all victorious) lists them all; malformed states count as out
		CHECK((replay_result::decide(slots({0, 1}), {5, 5}).winners == std::vector<int>{0, 1}));
		auto r = replay_result::decide(slots({0, 1}), {-3, 0});
		CHECK(r.players[0] == outcome::defeated && r.winners == std::vector<int>{1});
	}
	{  // nobody in the game
		auto r = replay_result::decide({}, {});
		CHECK(r.winners.empty());
	}
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
