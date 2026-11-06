#ifndef OPENBW_UI_MINIMAP_BLOCK_H
#define OPENBW_UI_MINIMAP_BLOCK_H

// A building on the minimap drawn as a block with height, in oblique projection: its roof,
// and below and to the right of it the two sides that face away from a top-left light. In a
// footprint of w x h pixels with depth d, the roof is the top-left (w - d) x (h - d) and the
// sides are parallelograms slanting down-right, so the top-right and bottom-left corners stay
// empty, as in a drawn cube. Pure: no game state, so it can be tested on its own
// (viewer/tests/minimap_block_test.cpp).

#include <algorithm>

namespace minimap_block {

enum class face { none, roof, right, front };

// The block's depth for a footprint: about a third of its shorter side, at least 1.5 px so
// the height shows on the smallest buildings and at most 4 px so it never swamps the roof.
inline double depth(double w, double h) {
	double s = std::min(w, h);
	if (!(s > 0)) return 0;
	return std::max(1.5, std::min(4.0, s * 0.32));
}

// The face at (u, v), relative to the footprint's top left.
inline face at(double u, double v, double w, double h, double d) {
	if (!(w > 0 && h > 0) || u < 0 || v < 0 || u >= w || v >= h) return face::none;
	d = std::max(0.0, std::min(d, std::min(w, h)));
	if (u < w - d && v < h - d) return face::roof;
	if (u >= w - d) {
		double t = u - (w - d);
		if (v >= t && v < h - d + t) return face::right;
	}
	if (v >= h - d) {
		double t = v - (h - d);
		if (u >= t && u <= w - d + t) return face::front;  // the seam with the right side belongs to the front
	}
	return face::none;
}

}

#endif
