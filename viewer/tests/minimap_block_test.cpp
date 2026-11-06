// Behaviour tests for minimap building blocks (ui/minimap_block.h).
// Build and run: cmake --build build --target openbw-minimap-block-test && ./build/openbw-minimap-block-test
#include "minimap_block.h"

#include <cmath>
#include <cstdio>
#include <limits>

using minimap_block::face;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	const double w = 10, h = 8, d = 3;
	// The roof is the top-left part; the right side and the front below it.
	CHECK(minimap_block::at(0.5, 0.5, w, h, d) == face::roof);
	CHECK(minimap_block::at(6.5, 4.5, w, h, d) == face::roof);
	CHECK(minimap_block::at(8.5, 4.5, w, h, d) == face::right);
	CHECK(minimap_block::at(4.5, 6.5, w, h, d) == face::front);
	// The two cut corners of a drawn cube are empty.
	CHECK(minimap_block::at(9.5, 0.2, w, h, d) == face::none);  // top right, above the slanted side
	CHECK(minimap_block::at(0.2, 7.5, w, h, d) == face::none);  // bottom left, left of the slanted front
	// The far bottom-right corner belongs to a side.
	CHECK(minimap_block::at(9.5, 7.5, w, h, d) != face::none);
	// Outside the footprint, or a degenerate one: nothing.
	CHECK(minimap_block::at(-0.1, 1, w, h, d) == face::none);
	CHECK(minimap_block::at(10, 1, w, h, d) == face::none);
	CHECK(minimap_block::at(1, 1, 0, 8, d) == face::none);
	CHECK(minimap_block::at(1, 1, w, h, std::numeric_limits<double>::quiet_NaN()) != face::right);
	// A depth larger than the footprint is clamped: no crash, still inside.
	CHECK(minimap_block::at(0.5, 0.5, 4, 4, 100) != face::roof);
	// Every point of the footprint is on at most one face, and the visible area is the roof
	// plus two sides of depth d: w*h minus the two cut triangles (d*d/2 each).
	int n = 0, roof = 0;
	for (double v = 0.05; v < h; v += 0.1) for (double u = 0.05; u < w; u += 0.1) {
		face f = minimap_block::at(u, v, w, h, d);
		n += f != face::none, roof += f == face::roof;
	}
	CHECK(std::abs(n / 100.0 - (w * h - d * d)) < 0.5);
	CHECK(std::abs(roof / 100.0 - (w - d) * (h - d)) < 0.5);
	// Depth: a third of the shorter side, within 1.5 and 4 px.
	CHECK(minimap_block::depth(6, 9) > 1.9 && minimap_block::depth(6, 9) < 2.0);
	CHECK(minimap_block::depth(2, 2) == 1.5);
	CHECK(minimap_block::depth(100, 100) == 4.0);
	CHECK(minimap_block::depth(0, 5) == 0);
	CHECK(minimap_block::depth(std::numeric_limits<double>::quiet_NaN(), 5) == 0);
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
