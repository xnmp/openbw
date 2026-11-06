// Behaviour tests for minimap building bevels (ui/minimap_bevel.h).
// Build and run: cmake --build build --target openbw-minimap-bevel-test && ./build/openbw-minimap-bevel-test
#include "minimap_bevel.h"

#include <cmath>
#include <cstdio>
#include <limits>

using minimap_bevel::part;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	const double w = 10, h = 8, b = 2;
	CHECK(minimap_bevel::at(5, 4, w, h, b) == part::face);       // the middle
	CHECK(minimap_bevel::at(5, 0.5, w, h, b) == part::lit);      // top band
	CHECK(minimap_bevel::at(0.5, 4, w, h, b) == part::lit);      // left band
	CHECK(minimap_bevel::at(5, 7.5, w, h, b) == part::shadow);   // bottom band
	CHECK(minimap_bevel::at(9.5, 4, w, h, b) == part::shadow);   // right band
	CHECK(minimap_bevel::at(5, 1.9, w, h, b) == part::lit);      // the band is b wide
	CHECK(minimap_bevel::at(5, 2.1, w, h, b) == part::face);
	// Mitred corners: the top-right and bottom-left corners split along their diagonals.
	CHECK(minimap_bevel::at(9.0, 0.5, w, h, b) == part::lit);     // nearer the top
	CHECK(minimap_bevel::at(9.7, 1.5, w, h, b) == part::shadow);  // nearer the right
	CHECK(minimap_bevel::at(0.5, 7.0, w, h, b) == part::lit);     // nearer the left
	CHECK(minimap_bevel::at(1.5, 7.7, w, h, b) == part::shadow);  // nearer the bottom
	// Outside, degenerate or malformed: nothing, or all face with no bevel.
	CHECK(minimap_bevel::at(-0.1, 4, w, h, b) == part::none);
	CHECK(minimap_bevel::at(10, 4, w, h, b) == part::none);
	CHECK(minimap_bevel::at(1, 1, 0, h, b) == part::none);
	CHECK(minimap_bevel::at(std::numeric_limits<double>::quiet_NaN(), 4, w, h, b) == part::none);
	CHECK(minimap_bevel::at(5, 4, w, h, 0) == part::face);
	CHECK(minimap_bevel::at(5, 4, w, h, std::numeric_limits<double>::quiet_NaN()) == part::face);
	// A bevel wider than half the tile leaves no face, and lit and shadow split it evenly.
	int lit = 0, shadow = 0, face = 0;
	for (double v = 0.05; v < 4; v += 0.1) for (double u = 0.05; u < 4; u += 0.1) {
		part p = minimap_bevel::at(u, v, 4, 4, 100);
		lit += p == part::lit, shadow += p == part::shadow, face += p == part::face;
	}
	CHECK(face == 0 && std::abs(lit - shadow) <= 40);
	// Width: a quarter of the shorter side, within 1.5 and 3 px.
	CHECK(minimap_bevel::width(8, 10) == 2.0);
	CHECK(minimap_bevel::width(2, 2) == 1.5);
	CHECK(minimap_bevel::width(100, 100) == 3.0);
	CHECK(minimap_bevel::width(0, 5) == 0);
	CHECK(minimap_bevel::width(std::numeric_limits<double>::quiet_NaN(), 5) == 0);
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
