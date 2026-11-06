// Behaviour tests for the viewer's background-music playlist (ui/music.h) and its player
// (ui/music_player.h, driven through a fake sound layer).
// Build and run: cmake --build build --target openbw-music-test && ./build/openbw-music-test
#include "music.h"

#include <algorithm>
#include <cstdio>
#include <set>

// Fake sound layer standing in for SDL_mixer: a track "plays" until the test finishes it.
namespace native_music {
	bool mixer = true;
	int plays = 0, volume = -1;
	bool playing = false, paused = false;
	std::vector<uint8_t> last;
	bool available() { return mixer; }
	bool play(std::vector<uint8_t> d) { last = d; playing = true; ++plays; return true; }
	bool is_playing() { return playing; }
	void stop() { playing = false; }
	void set_volume(int v) { volume = v; }
	void set_paused(bool p) { paused = p; }
}
#include "music_player.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using music::race;

static std::vector<long> draw(music::shuffler& s, int n) {
	std::vector<long> v;
	for (int i = 0; i != n; ++i) v.push_back(s.next());
	return v;
}

int main() {
	{  // tracks follow the races present; duplicates and order of the input do not matter
		auto t = music::candidate_tracks({race::zerg});
		CHECK(t.size() == 4 && t.front() == "zerg1" && t.back() == "zerg4");
		auto both = music::candidate_tracks({race::zerg, race::terran, race::zerg});
		CHECK(both.size() == 8);
		CHECK(both == music::candidate_tracks({race::terran, race::zerg}));
		CHECK(std::set<std::string>(both.begin(), both.end()).size() == 8);
	}
	{  // unknown races (empty) means the whole soundtrack
		CHECK(music::candidate_tracks({}).size() == 12);
	}
	{  // no tracks: nothing to play, no crash
		music::shuffler s(0, 1);
		CHECK(s.next() == -1 && s.next() == -1);
	}
	{  // a single track repeats (the only option)
		music::shuffler s(1, 7);
		CHECK(draw(s, 3) == std::vector<long>({0, 0, 0}));
	}
	{  // every track plays once per cycle, and no track plays twice in a row, across many cycles
		for (uint64_t seed = 0; seed != 200; ++seed) {
			for (size_t n : {2u, 3u, 4u, 8u, 12u}) {
				music::shuffler s(n, seed);
				auto v = draw(s, (int)n * 6);
				for (size_t c = 0; c != 6; ++c) {
					std::set<long> cycle(v.begin() + c * n, v.begin() + (c + 1) * n);
					CHECK(cycle.size() == n);
				}
				for (size_t i = 1; i != v.size(); ++i) CHECK(v[i] != v[i - 1]);
				CHECK(*std::min_element(v.begin(), v.end()) == 0 && *std::max_element(v.begin(), v.end()) == (long)n - 1);
			}
		}
	}
	{  // seedable: same seed same order, different seeds differ somewhere
		music::shuffler a(12, 42), b(12, 42), c(12, 43);
		CHECK(draw(a, 36) == draw(b, 36));
		music::shuffler a2(12, 42);
		CHECK(draw(a2, 36) != draw(c, 36));
	}
	{  // volume: music is quieter than effects, follows the master volume, and mute silences it
		int effects = (128 - 4) * 50 / 100;
		CHECK(music::mix_volume(true, 50) > 0 && music::mix_volume(true, 50) < effects);
		CHECK(music::mix_volume(true, 100) > music::mix_volume(true, 50));
		CHECK(music::mix_volume(true, 0) == 0);
		CHECK(music::mix_volume(false, 100) == 0);
		CHECK(music::mix_volume(true, 1000) == music::mix_volume(true, 100));  // out of range clamps
		CHECK(music::mix_volume(true, -5) == 0);
		CHECK(music::mix_volume(true, 100) <= 128);
	}
	{  // player: plays a track for the races, advances when it ends, never repeats back to back
		native_music::plays = 0; native_music::playing = false;
		std::vector<std::string> loaded;
		music::player p;
		p.start({race::protoss}, [&](const std::string& n, std::vector<uint8_t>& out) { loaded.push_back(n); out = {1}; return true; }, 5);
		CHECK(native_music::plays == 0);  // nothing starts until update()
		for (int i = 0; i != 9; ++i) { p.update(true, 50); native_music::playing = false; }
		CHECK(native_music::plays == 9);
		CHECK(loaded.size() == 9);
		for (auto& n : loaded) CHECK(n.rfind("protoss", 0) == 0);
		for (size_t i = 1; i != loaded.size(); ++i) CHECK(loaded[i] != loaded[i - 1]);
	}
	{  // player: a playing track is not interrupted by further updates
		native_music::plays = 0; native_music::playing = false;
		music::player p;
		p.start({}, [](const std::string&, std::vector<uint8_t>& out) { out = {1}; return true; }, 1);
		for (int i = 0; i != 10; ++i) p.update(true, 50);
		CHECK(native_music::plays == 1);
	}
	{  // player: disabled or muted pauses without losing the track; re-enabling resumes it
		native_music::plays = 0; native_music::playing = false;
		music::player p;
		p.start({race::zerg}, [](const std::string&, std::vector<uint8_t>& out) { out = {1}; return true; }, 1);
		p.update(true, 50);
		CHECK(native_music::plays == 1 && !native_music::paused && native_music::volume > 0);
		p.update(false, 50);
		CHECK(native_music::paused && native_music::volume == 0);
		p.update(true, 0);
		CHECK(native_music::paused);
		p.update(true, 50);
		CHECK(!native_music::paused && native_music::volume > 0 && native_music::plays == 1);
	}
	{  // player: a muted player does not start a new track when one ends
		native_music::plays = 0; native_music::playing = false;
		music::player p;
		p.start({race::zerg}, [](const std::string&, std::vector<uint8_t>& out) { out = {1}; return true; }, 1);
		p.update(false, 50);
		CHECK(native_music::plays == 0);
	}
	{  // player: unreadable tracks are skipped; if none are readable it goes quiet without retrying
		native_music::plays = 0; native_music::playing = false;
		int calls = 0;
		music::player p;
		p.start({race::terran}, [&](const std::string& n, std::vector<uint8_t>& out) { ++calls; out = {1}; return n == "terran3"; }, 9);
		p.update(true, 50);
		CHECK(native_music::plays == 1);
		native_music::playing = false;
		p.update(true, 50);
		CHECK(native_music::plays == 2);  // terran3 again: the only readable track
		native_music::playing = false;
		int before = calls;
		music::player q;
		q.start({race::terran}, [&](const std::string&, std::vector<uint8_t>&) { ++calls; return false; }, 9);
		for (int i = 0; i != 5; ++i) q.update(true, 50);
		CHECK(calls - before == 4);  // each of the 4 tracks tried once, then no more attempts
		CHECK(native_music::plays == 2);
	}
	{  // player: no mixer means it never touches the files; stop() is safe at any time
		native_music::mixer = false;
		int calls = 0;
		music::player p;
		p.start({}, [&](const std::string&, std::vector<uint8_t>&) { ++calls; return true; }, 1);
		p.update(true, 50);
		CHECK(calls == 0);
		native_music::mixer = true;
		p.stop();
		p.stop();
		p.update(true, 50);  // not started: no-op
		music::player never;
		never.stop();
	}
	if (failures == 0) std::printf("all passed\n");
	return failures != 0;
}
