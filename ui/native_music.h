#ifndef NATIVE_MUSIC_H
#define NATIVE_MUSIC_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Streaming music through the platform sound layer (SDL2_mixer in sdl2.cpp). One track at a
// time, independent of the sound-effect channels. All no-ops when built without a mixer.
namespace native_music {
	bool available();  // false when built without a mixer
	// Starts playing an encoded (ogg/wav) track once, replacing the current one. Takes the
	// bytes because the decoder streams from them. False if it cannot be decoded.
	bool play(std::vector<uint8_t> data);
	bool is_playing();  // true while a track is loaded and unfinished, also when paused
	void stop();
	void set_volume(int volume);  // 0..128
	void set_paused(bool paused);
}

#endif
