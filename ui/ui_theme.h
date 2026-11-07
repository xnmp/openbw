#ifndef OPENBW_UI_THEME_H
#define OPENBW_UI_THEME_H

// The viewer's UI theme: which race's look the observer skin wears, and the colours that
// make it. Pure (no game data, no drawing) so the rules can be tested on their own.
//
// Remastered ships race art only for the in-game consoles and their progress bars, not for
// the observer panels, so the panels, minimap frame, buttons and command icons are the one
// observer art recoloured toward each race (tint_rgb); the bars use the race's own art.

#include <cstdint>

namespace ui_theme {

enum class race { terran = 0, protoss = 1, zerg = 2 };

// The setting is a level 1..5 in the menu and the settings file. Minimal (the default) uses
// no game art: flat panels, hairline borders, neutral greys and one blue accent (`minimal`
// below); it is also the look without a Remastered install. The race themes wear the
// observer art. Levels 1..4 kept their numbers when Minimal was added, so saved choices hold.
enum { level_auto = 1, level_terran = 2, level_protoss = 3, level_zerg = 4, level_minimal = 5, level_count = 5 };
constexpr int level_default = level_minimal;

inline int clamp_level(int level) { return level < 1 || level > level_count ? level_default : level; }

// Whether a level wears the game's art (the race themes) rather than the Minimal look.
inline bool styled(int level) { return clamp_level(level) != level_minimal; }

// The game's race numbering (0 Zerg, 1 Terran, 2 Protoss) to a theme race; false if it is
// not one of the three (Random, Inactive, None).
inline bool from_game_race(int game_race, race& out) {
	switch (game_race) {
	case 0: out = race::zerg; return true;
	case 1: out = race::terran; return true;
	case 2: out = race::protoss; return true;
	}
	return false;
}

// The race a level stands for (the music and the race-themed art follow it). Auto, and
// Minimal, which has no race of its own, follow the first player in the replay (lowest slot,
// the one a viewer reads as "the player"): stable across the game, unlike following the
// selection. `game_races` lists the races of the active players in slot order. A fixed race
// level wins; with no usable player the race is Terran, the skin's own look.
template <typename It>
race resolve(int level, It first, It last) {
	switch (clamp_level(level)) {
	case level_terran: return race::terran;
	case level_protoss: return race::protoss;
	case level_zerg: return race::zerg;
	}
	for (; first != last; ++first) {
		race r;
		if (from_game_race((int)*first, r)) return r;
	}
	return race::terran;
}

struct rgb { int r, g, b; };

struct palette {
	const char* console_dir;  // "HD2/game/consoles/<dir>/": the race's own pbrempt/pbrfull bars
	bool recolour;            // false: the observer art as it is (Terran)
	rgb metal;                // per-channel gain on the art's grey metal, 256 = unchanged
	rgb accent;               // what the art's red lines and rivets become
	rgb icon_mid, icon_high;  // command icons: black, through icon_mid at mid grey, to icon_high
	rgb button_top, button_bottom, button_hover_top, button_hover_bottom;
	rgb glow;                 // hovered-button rim
};

inline const palette& palette_of(race r) {
	static const palette terran = {"terran", false, {256, 256, 256}, {200, 30, 30}, {236, 196, 36}, {255, 255, 190},
		{62, 68, 70}, {18, 21, 23}, {84, 82, 74}, {28, 26, 22}, {240, 170, 60}};
	static const palette protoss = {"protoss", true, {205, 250, 345}, {40, 165, 255}, {70, 170, 255}, {205, 238, 255},
		{44, 62, 90}, {10, 16, 30}, {58, 84, 116}, {14, 24, 42}, {90, 190, 255}};
	static const palette zerg = {"zerg", true, {320, 225, 175}, {245, 105, 30}, {236, 98, 40}, {255, 214, 160},
		{84, 52, 40}, {26, 15, 12}, {108, 66, 46}, {38, 20, 14}, {255, 120, 40}};
	return r == race::protoss ? protoss : r == race::zerg ? zerg : terran;
}

// The Minimal theme's colours.
namespace minimal {
constexpr rgb panel{15, 17, 21};  // panel fill (drawn about 90% opaque)
constexpr rgb border{58, 63, 72};  // a panel's hairline edge
constexpr rgb separator{36, 40, 48};
constexpr rgb title{246, 247, 250}, text{226, 229, 235}, dim{136, 143, 155};
constexpr rgb accent{96, 165, 250};  // keys, the seek bar's fill, "on"
constexpr rgb track{34, 37, 44};     // a bar's empty part
constexpr rgb button{30, 33, 40}, button_hover{44, 49, 59}, button_pressed{22, 24, 29};
constexpr rgb grid{40, 44, 52}, tick{29, 32, 38};
}  // namespace minimal

inline int clamp_channel(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

// Recolours one pixel of the observer art (premultiplied, alpha `a`): the red parts (inner
// line, rivets) take the accent, scaled by how red they are and how bright; the rest keeps
// its brightness, tinted by the metal gain. Channels never exceed alpha, so premultiplied
// stays valid.
inline rgb tint_rgb(int r, int g, int b, int a, const palette& p) {
	if (!p.recolour) return {r, g, b};
	int mx = g > b ? g : b;
	int redness = r > mx ? r - mx : 0;
	int k = clamp_channel(redness * 256 / 90, 256);  // 0..256
	int base = (g + b) / 2;
	auto ch = [&](int gain, int accent) { return clamp_channel(base * gain / 256 + accent * k * r / (255 * 256), a); };
	return {ch(p.metal.r, p.accent.r), ch(p.metal.g, p.accent.g), ch(p.metal.b, p.accent.b)};
}

// A command icon's grey (0..255) as the theme's ramp.
inline rgb icon_color(int grey, const palette& p) {
	int g = clamp_channel(grey, 255);
	if (g < 128) return {p.icon_mid.r * g / 128, p.icon_mid.g * g / 128, p.icon_mid.b * g / 128};
	int t = g - 128;
	return {p.icon_mid.r + (p.icon_high.r - p.icon_mid.r) * t / 127, p.icon_mid.g + (p.icon_high.g - p.icon_mid.g) * t / 127,
	        p.icon_mid.b + (p.icon_high.b - p.icon_mid.b) * t / 127};
}

}  // namespace ui_theme

#endif
