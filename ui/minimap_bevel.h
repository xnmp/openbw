#ifndef OPENBW_UI_MINIMAP_BEVEL_H
#define OPENBW_UI_MINIMAP_BEVEL_H

// A building on the minimap drawn as a bevelled tile lit from the top left: a band of width
// b along the top and left edges catches the light, a band along the bottom and right edges
// is in shadow, and the face sits between them. Each point of the band belongs to its nearest
// edge, so the corners are mitred as in a picture frame. Pure: no game state, so it can be
// tested on its own (viewer/tests/minimap_bevel_test.cpp).

#include <algorithm>

namespace minimap_bevel {

enum class part { none, face, lit, shadow };

// The bevel's width for a footprint: about a quarter of its shorter side, at least 1.5 px so
// it shows on the smallest buildings, at most 3 px so it never swamps the face.
inline double width(double w, double h) {
	double s = std::min(w, h);
	if (!(s > 0)) return 0;
	return std::max(1.5, std::min(3.0, s * 0.25));
}

// The part at (u, v), relative to the footprint's top left.
inline part at(double u, double v, double w, double h, double b) {
	if (!(w > 0 && h > 0) || !(u >= 0 && v >= 0 && u < w && v < h)) return part::none;
	if (!(b > 0)) return part::face;
	double top = v, left = u, bottom = h - v, right = w - u;
	double lit = std::min(top, left), dark = std::min(bottom, right);
	if (std::min(lit, dark) >= b) return part::face;
	return lit <= dark ? part::lit : part::shadow;
}

}

#endif
