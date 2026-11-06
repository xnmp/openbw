#ifndef OPENBW_UI_HEALTH_BAR_H
#define OPENBW_UI_HEALTH_BAR_H

// Layout and colour rules for the Remastered view's health bars: how wide a bar is on screen,
// how many pips it is divided into, where the shield / hit point / energy rows sit, and the
// colour of a fill. Pure (no game state, no drawing), so it can be tested on its own
// (viewer/tests/health_bar_test.cpp). The renderer in ui.h draws what this describes at window
// resolution, so bars stay crisp at every zoom step.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace health_bar {

template <class T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

struct rgb {
	int r, g, b;
	bool operator==(const rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

// Bar width in screen pixels. `map_width` is the sprite type's health_bar_size; it is
// normalised as the game does (a multiple of 3 plus 1, at least 19) and scaled by the zoom.
inline int bar_width(int map_width, double scale) {
	int w = map_width - (map_width - 1) % 3;
	if (w < 19) w = 19;
	return std::max(1, (int)std::lround(w * scale));
}

// Pips in a bar: one per ~25 hit points, so a Marine reads as 2 and a Zergling as 2, a Zealot
// 4, an Ultralisk 16 -> capped at 12 so a 1500 hp building is not tick soup, and never
// narrower than 4 px each.
inline int segment_count(int max_hp, int width_px) {
	int n = clamp((int)std::lround(max_hp / 25.0), 2, 12);
	return std::max(1, std::min(n, width_px / 4));
}

// Filled width for a fraction of the bar: empty only at 0, never a sliver below 1 px,
// full only at 1.
inline int fill_width(double frac, int width_px) {
	if (!(frac > 0)) return 0;
	if (frac >= 1) return width_px;
	return std::min(width_px - 1, std::max(1, (int)std::lround(frac * width_px)));
}

// Offset of the boundary after pip `i` (1 <= i < segments); boundaries are evenly spaced.
inline int tick_x(int i, int segments, int width_px) {
	return (int)std::lround((double)i * width_px / std::max(1, segments));
}

// Hit point colour: red below a third, amber at a third, green from two thirds, blended
// linearly in between (the game's 33 / 66 steps, smoothed).
inline rgb hp_color(double frac) {
	const rgb red{222, 52, 48}, amber{244, 190, 48}, green{64, 204, 76};
	auto mix = [](rgb a, rgb b, double t) {
		return rgb{(int)std::lround(a.r + (b.r - a.r) * t), (int)std::lround(a.g + (b.g - a.g) * t), (int)std::lround(a.b + (b.b - a.b) * t)};
	};
	frac = clamp(frac, 0.0, 1.0);
	if (frac <= 1.0 / 3) return mix(red, amber, frac * 3);
	if (frac >= 2.0 / 3) return green;
	return mix(amber, green, (frac - 1.0 / 3) * 3);
}
inline rgb shield_color() { return {72, 156, 255}; }
inline rgb energy_color() { return {196, 84, 232}; }
inline rgb track_color() { return {26, 30, 38}; }
inline rgb outline_color() { return {6, 8, 12}; }

// Brightness change of a row by `percent` (+ lighter toward white, - darker toward black).
inline rgb shade(rgb c, int percent) {
	auto f = [&](int v) {
		return percent >= 0 ? v + (255 - v) * percent / 100 : v + v * percent / 100;
	};
	return {f(c.r), f(c.g), f(c.b)};
}

// Vertical shading of row line `j` of `h`: a lit top edge, then a gentle fall to a darker
// bottom. Rows of 1-2 px stay flat apart from the top highlight.
inline int row_shade(int j, int h) {
	if (j == 0) return h >= 2 ? 34 : 0;
	if (h <= 2) return 0;
	return 10 - 30 * (j - 1) / std::max(1, h - 2);
}

struct row {
	int y = 0;  // from the top of the bar's outline
	int h = 0;  // 0: no such row
};
struct layout_t {
	row shield, hp, energy;
	int total_h = 0;  // including the outline
	int pad = 1;      // outline thickness
};

// Rows top to bottom: shield (Protoss), hit points, energy (casters), separated by a 1 px
// line and wrapped in a 1 px outline. Thickness follows the zoom (a 3 px hit point row at
// 1x, 10 at 4x); the shield and energy rows are slimmer.
inline layout_t layout(bool shield, bool energy, double scale) {
	layout_t l;
	int hp_h = clamp((int)std::lround(2.5 * scale), 3, 10);
	int side_h = std::max(2, (int)std::lround(hp_h * 0.6));
	int y = l.pad;
	if (shield) {
		l.shield = {y, side_h};
		y += side_h + 1;
	}
	l.hp = {y, hp_h};
	y += hp_h;
	if (energy) {
		y += 1;
		l.energy = {y, side_h};
		y += side_h;
	}
	l.total_h = y + l.pad;
	return l;
}

// A bar at full health, shield and energy is drawn fainter so a crowd of healthy units is
// quiet and damaged ones stand out (alpha 0..255).
inline int bar_alpha(bool all_full) { return all_full ? 190 : 255; }

}  // namespace health_bar

#endif
