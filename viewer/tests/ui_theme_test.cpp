// Behaviour tests for the UI theme rules (ui/ui_theme.h).
// Build and run: cmake --build build --target openbw-ui-theme-test && ./build/openbw-ui-theme-test
#include "ui_theme.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ui_theme;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

enum { zerg = 0, terran = 1, protoss = 2, random_race = 6, none = 5 };

static race pick(int level, std::vector<int> races) { return resolve(level, races.begin(), races.end()); }

int main() {
	// Auto follows the first player; the second player does not matter.
	CHECK(pick(level_auto, {zerg, protoss}) == race::zerg);
	CHECK(pick(level_auto, {protoss, zerg}) == race::protoss);
	CHECK(pick(level_auto, {terran}) == race::terran);
	// A slot with no usable race is skipped; with none, Terran.
	CHECK(pick(level_auto, {random_race, protoss}) == race::protoss);
	CHECK(pick(level_auto, {none, -1, 99}) == race::terran);
	CHECK(pick(level_auto, {}) == race::terran);
	// A fixed theme ignores the players.
	CHECK(pick(level_zerg, {protoss}) == race::zerg);
	CHECK(pick(level_protoss, {zerg}) == race::protoss);
	CHECK(pick(level_terran, {zerg}) == race::terran);
	// A bad level (hand-edited settings file) falls back to Auto.
	CHECK(pick(0, {protoss}) == race::protoss);
	CHECK(pick(99, {zerg}) == race::zerg);
	CHECK(pick(-5, {zerg}) == race::zerg);

	// Each race has its own bar art directory.
	CHECK(std::string(palette_of(race::zerg).console_dir) == "zerg");
	CHECK(std::string(palette_of(race::protoss).console_dir) == "protoss");
	CHECK(std::string(palette_of(race::terran).console_dir) == "terran");

	// Terran leaves the observer art alone.
	{
		rgb t = tint_rgb(120, 90, 80, 255, palette_of(race::terran));
		CHECK(t.r == 120 && t.g == 90 && t.b == 80);
	}
	// A red rivet turns toward the accent: Protoss blue, Zerg orange.
	{
		rgb p = tint_rgb(220, 40, 40, 255, palette_of(race::protoss));
		CHECK(p.b > p.r && p.b > 100);
		rgb z = tint_rgb(220, 40, 40, 255, palette_of(race::zerg));
		CHECK(z.r > z.b && z.r > 150 && z.g > z.b);
	}
	// Grey metal stays roughly neutral in brightness but takes the race's cast.
	{
		rgb p = tint_rgb(100, 100, 100, 255, palette_of(race::protoss));
		CHECK(p.b > p.r);
		rgb z = tint_rgb(100, 100, 100, 255, palette_of(race::zerg));
		CHECK(z.r > z.b);
	}
	// Premultiplied output never exceeds alpha, and fully transparent stays black.
	for (int a : {0, 1, 40, 128, 255})
		for (race r : {race::protoss, race::zerg})
			for (int v : {0, 30, 200, 255}) {
				int c = v < a ? v : a;
				rgb o = tint_rgb(c, c / 3, c / 3, a, palette_of(r));
				CHECK(o.r <= a && o.g <= a && o.b <= a && o.r >= 0 && o.g >= 0 && o.b >= 0);
			}
	// The icon ramp: black stays black, mid grey is the theme's mid colour, white its high.
	for (race r : {race::terran, race::protoss, race::zerg}) {
		const palette& p = palette_of(r);
		rgb k = icon_color(0, p), w = icon_color(255, p);
		CHECK(k.r == 0 && k.g == 0 && k.b == 0);
		CHECK(w.r == p.icon_high.r && w.g == p.icon_high.g && w.b == p.icon_high.b);
		rgb m = icon_color(128, p);
		CHECK(m.r == p.icon_mid.r && m.g == p.icon_mid.g && m.b == p.icon_mid.b);
		rgb over = icon_color(1000, p), under = icon_color(-9, p);
		CHECK(over.r == p.icon_high.r && under.r == 0);
	}
	// The Terran ramp is the console gold the icons always had.
	{
		rgb m = icon_color(128, palette_of(race::terran));
		CHECK(m.r == 236 && m.g == 196 && m.b == 36);
	}

	if (failures) return 1;
	std::printf("all passed\n");
	return 0;
}
