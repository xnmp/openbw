#ifndef OPENBW_SKIN_FRAME_H
#define OPENBW_SKIN_FRAME_H

// A race theme's panel frame, cut from the frame around the main screen in the race's
// console art (HD2/game/consoles/<race>/console_center.dds): the Protoss blue bezel, the Zerg
// flesh ring. The art has one top-left corner and a run of the top and left edges worth
// keeping; the other three corners and edges are its mirror images, so the frame is
// symmetric. Each edge run is stored followed by its own mirror, so repeating it end to end
// has no seam. Pure pixel work on premultiplied RGBA (alpha the top byte); no decoding.
//
// The atlas is a 3 x 3 grid: corners `corner` square, the top/bottom edges `edge_x` wide,
// the left/right edges `edge_y` tall, the centre transparent:
//
//   [TL corner][top edge  ][TR corner]
//   [left edge][          ][right edge]
//   [BL corner][bottom    ][BR corner]

#include <cstddef>
#include <cstdint>
#include <vector>

namespace skin_frame {

struct spec {
	int x = 0, y = 0;  // the top-left corner's top-left pixel in the source
	int corner = 0;  // the corner's side, in source pixels
	int top_from = 0, top_to = 0;  // columns [from, to) of the top edge, over the corner's rows
	int left_from = 0, left_to = 0;  // rows [from, to) of the left edge, over the corner's columns
	int inset_top = 0, inset_side = 0;  // the frame's thickness at the top (and bottom) and sides
};

struct layout {
	int corner = 0, edge_x = 0, edge_y = 0;
	int w() const { return 2 * corner + edge_x; }
	int h() const { return 2 * corner + edge_y; }
};

// Scales a spec's coordinates (a source resampled by `s`), rounding to whole pixels and
// keeping every run at least one pixel long.
inline spec scaled(const spec& a, double s) {
	auto r = [&](int v) { return (int)(v * s + 0.5); };
	auto run = [&](int from, int to, int& f, int& t) { f = r(from); t = r(to); if (t <= f) t = f + 1; };
	spec b;
	b.x = r(a.x); b.y = r(a.y);
	b.corner = r(a.corner) > 0 ? r(a.corner) : 1;
	run(a.top_from, a.top_to, b.top_from, b.top_to);
	run(a.left_from, a.left_to, b.left_from, b.left_to);
	b.inset_top = (int)(a.inset_top * s + 0.999);
	b.inset_side = (int)(a.inset_side * s + 0.999);
	return b;
}

// Whether the spec's regions lie inside a w x h source.
inline bool fits(const spec& s, int w, int h) {
	return s.corner > 0 && s.x >= 0 && s.y >= 0 && s.x + s.corner <= w && s.y + s.corner <= h &&
	       s.top_from >= 0 && s.top_to > s.top_from && s.top_to <= w &&
	       s.left_from >= 0 && s.left_to > s.left_from && s.left_to <= h;
}

// The source coordinate on one axis for atlas position `a`: near (mirrored at the far end)
// within the corners, else in the edge run (forward, then backward). `mid` says which.
inline int axis(int a, int corner, int run_from, int run_n, int origin, int total, bool& mid) {
	mid = false;
	if (a < corner) return origin + a;
	if (a >= total - corner) return origin + (total - 1 - a);
	mid = true;
	int k = a - corner;
	return run_from + (k < run_n ? k : 2 * run_n - 1 - k);
}

// Builds the atlas from `src`. False (out untouched) when the regions do not fit.
template <class Img>
bool build(const Img& src, const spec& s, Img& out, layout& lay) {
	if (!fits(s, src.w, src.h)) return false;
	const int c = s.corner, nx = s.top_to - s.top_from, ny = s.left_to - s.left_from;
	layout l{c, 2 * nx, 2 * ny};
	const int W = l.w(), H = l.h();
	std::vector<uint32_t> px((size_t)W * H, 0);
	for (int y = 0; y != H; ++y) {
		bool my;
		int sy = axis(y, c, s.left_from, ny, s.y, H, my);
		int ly = y < c ? y : H - 1 - y;  // the row within the corner, for an edge's cross-section
		for (int x = 0; x != W; ++x) {
			bool mx;
			int sx = axis(x, c, s.top_from, nx, s.x, W, mx);
			int lx = x < c ? x : W - 1 - x;
			if (mx && my) continue;  // the centre
			int px_x = mx ? sx : s.x + lx, px_y = my ? sy : s.y + ly;
			if (!mx && !my) px_x = sx, px_y = sy;
			px[(size_t)y * W + x] = src.px[(size_t)px_y * src.w + px_x];
		}
	}
	out.w = W;
	out.h = H;
	out.px = std::move(px);
	lay = l;
	return true;
}

// Makes the art's screen transparent: from the empty centre, every connected pixel no
// brighter than `max_channel` in any channel (the near-black screen inside the frame) is
// cleared, stopping at the frame's own rails and rims. For a frame drawn over something
// that must show through it (the minimap).
template <class Img>
void open_face(Img& a, const layout& l, int max_channel) {
	if (a.w != l.w() || a.h != l.h() || a.px.empty()) return;
	auto dark = [&](uint32_t p) {
		return (int)(p & 255) <= max_channel && (int)(p >> 8 & 255) <= max_channel && (int)(p >> 16 & 255) <= max_channel;
	};
	std::vector<int> stack;
	std::vector<uint8_t> seen(a.px.size(), 0);
	for (int y = l.corner; y != a.h - l.corner; ++y)
		for (int x = l.corner; x != a.w - l.corner; ++x) {
			stack.push_back(y * a.w + x);
			seen[(size_t)y * a.w + x] = 1;
		}
	while (!stack.empty()) {
		int i = stack.back();
		stack.pop_back();
		a.px[i] = 0;
		int x = i % a.w, y = i / a.w;
		const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
		for (int k = 0; k != 4; ++k) {
			if (nx[k] < 0 || ny[k] < 0 || nx[k] >= a.w || ny[k] >= a.h) continue;
			int j = ny[k] * a.w + nx[k];
			if (seen[j] || !dark(a.px[j])) continue;
			seen[j] = 1;
			stack.push_back(j);
		}
	}
}

}  // namespace skin_frame

#endif
