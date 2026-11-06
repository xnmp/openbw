#ifndef OPENBW_MUSIC_H
#define OPENBW_MUSIC_H

// Pure playlist logic for the viewer's background music: which tracks exist for the races in a
// replay, which one plays next, and how loud music is. No game state, no SDL, no file access,
// so it is unit-testable (viewer/tests/music_test.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace music {

enum class race { terran, protoss, zerg };

constexpr int tracks_per_race = 4;

inline const char* race_prefix(race r) {
	switch (r) {
	case race::terran: return "terran";
	case race::protoss: return "protoss";
	case race::zerg: return "zerg";
	}
	return "";
}

// Base names ("terran1" .. "zerg4"; the caller adds the directory and extension) of the
// soundtrack for the given races. Duplicates in `races` are ignored. An empty `races` means
// every race, as when a replay's races are unknown (random, observer-only).
inline std::vector<std::string> candidate_tracks(std::vector<race> races) {
	if (races.empty()) races = {race::terran, race::protoss, race::zerg};
	std::vector<std::string> out;
	for (race r : {race::terran, race::protoss, race::zerg}) {
		if (std::find(races.begin(), races.end(), r) == races.end()) continue;
		for (int i = 1; i <= tracks_per_race; ++i) out.push_back(race_prefix(r) + std::to_string(i));
	}
	return out;
}

// Deterministic for a given seed on every platform (std::shuffle is not).
struct rng {
	uint64_t state;
	explicit rng(uint64_t seed) : state(seed) {}
	uint64_t next() {  // splitmix64
		uint64_t z = (state += 0x9e3779b97f4a7c15ull);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}
	size_t below(size_t n) { return (size_t)(next() % n); }
};

// Shuffle-bag order over `count` tracks: every track plays once per cycle, in a random order,
// and a new cycle never starts with the track that ended the previous one.
class shuffler {
public:
	shuffler(size_t count, uint64_t seed) : count_(count), rng_(seed) {}

	size_t count() const { return count_; }

	// Index of the track to play next, or -1 when there are no tracks.
	long next() {
		if (count_ == 0) return -1;
		if (pos_ == order_.size()) refill();
		return last_ = (long)order_[pos_++];
	}

private:
	void refill() {
		order_.resize(count_);
		for (size_t i = 0; i != count_; ++i) order_[i] = i;
		for (size_t i = count_; i > 1; --i) std::swap(order_[i - 1], order_[rng_.below(i)]);
		if (count_ > 1 && last_ >= 0 && order_[0] == (size_t)last_) std::swap(order_[0], order_[1 + rng_.below(count_ - 1)]);
		pos_ = 0;
	}

	size_t count_;
	rng rng_;
	std::vector<size_t> order_;
	size_t pos_ = 0;
	long last_ = -1;
};

// SDL_mixer volume (0..128) for music. `global_volume` is the viewer's 0..100 master volume
// (0 is "muted"), so muting silences music too. Music sits at about a third of the sound
// effects' level (they use (128 - 4) * global_volume / 100), as games default music lower.
// A disabled music setting is silence regardless of the master volume.
inline int mix_volume(bool enabled, int global_volume) {
	if (!enabled) return 0;
	global_volume = std::max(0, std::min(100, global_volume));
	return (128 - 4) * global_volume / 100 * 35 / 100;
}

}

#endif
