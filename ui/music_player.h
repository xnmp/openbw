#ifndef OPENBW_MUSIC_PLAYER_H
#define OPENBW_MUSIC_PLAYER_H

// Glue between the playlist (music.h) and the sound layer (native_music.h): plays the soundtrack
// of a replay's races in shuffled order, moving on when a track ends. Poll update() every frame.

#include "music.h"
#include "native_music.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace music {

class player {
public:
	// Reads the file for a track base name such as "terran1" into `out` (the caller decides the
	// directory and extension, and which data source it comes from). False if it is missing.
	using loader = std::function<bool(const std::string& base_name, std::vector<uint8_t>& out)>;

	// Begins (or restarts) the soundtrack for `races`. Does nothing without a mixer.
	void start(const std::vector<race>& races, loader load, uint64_t seed) {
		stop();
		if (!native_music::available()) return;
		tracks_ = candidate_tracks(races);
		load_ = std::move(load);
		order_ = std::make_unique<shuffler>(tracks_.size(), seed);
		failed_.assign(tracks_.size(), false);
		running_ = true;
	}

	void stop() {
		if (running_) native_music::stop();
		running_ = false;
		order_.reset();
		load_ = nullptr;
		tracks_.clear();
	}

	// Call every frame. `enabled` is the music setting, `global_volume` the master 0..100 volume.
	// Playback pauses (and keeps its place) while the volume is zero; it does not depend on
	// whether the replay itself is paused.
	void update(bool enabled, int global_volume) {
		if (!running_) return;
		int volume = mix_volume(enabled, global_volume);
		if (volume != volume_) native_music::set_volume(volume_ = volume);
		bool silent = volume == 0;
		if (silent != paused_) native_music::set_paused(paused_ = silent);
		if (!silent && !native_music::is_playing()) play_next();
	}

private:
	void play_next() {
		// Unreadable tracks are skipped for good. Two cycles of draws is enough to reach every
		// track even when the current cycle is nearly spent.
		for (size_t tries = 0; tries != 2 * tracks_.size(); ++tries) {
			long i = order_->next();
			if (i < 0 || failed_[i]) continue;
			std::vector<uint8_t> data;
			if (load_(tracks_[i], data) && native_music::play(std::move(data))) return;
			failed_[i] = true;
		}
		running_ = false;  // nothing playable: stay quiet rather than retry every frame
	}

	std::vector<std::string> tracks_;
	std::vector<bool> failed_;
	loader load_;
	std::unique_ptr<shuffler> order_;
	bool running_ = false;
	bool paused_ = false;
	int volume_ = -1;
};

}

#endif
