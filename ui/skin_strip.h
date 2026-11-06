#ifndef OPENBW_SKIN_STRIP_H
#define OPENBW_SKIN_STRIP_H

// Builds a panel strip for a race's UI theme out of the race's own console art
// (HD2/game/consoles/<race>/console_*.dds): a rail along the top, a body that repeats, and a
// lip along the bottom, in the 230-row layout the observer panels use. Source regions are
// small, so each is repeated by reflection (a mirrored repeat has no visible seam).
// Pure pixel work on premultiplied RGBA (r low byte, alpha the top byte); no decoding.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace skin_strip {

struct box { int x0, y0, x1, y1; };  // [x0, x1) x [y0, y1)

// Index into n samples that bounces at both ends: 0..n-1, n-1..0, 0..n-1, ... (i >= 0).
inline int mirror_index(int i, int n) {
	if (n <= 1) return 0;
	int m = i % (2 * n);
	return m < n ? m : 2 * n - 1 - m;
}

// Scales the colour channels of a premultiplied pixel (alpha kept), gain in 0..256.
inline uint32_t shade(uint32_t p, int gain) {
	auto ch = [&](int s) { return (uint32_t)((((p >> s) & 255) * (uint32_t)gain) >> 8); };
	return (p & 0xff000000u) | ch(0) | ch(8) << 8 | ch(16) << 16;
}

// Fills dst rows [y0, y0 + rows) of width W from `src` region `b`, reflected in both axes.
// Does nothing when the region is empty or outside the source.
template <class Img>
bool reflect_fill(const Img& src, const box& b, std::vector<uint32_t>& dst, int W, int y0, int rows, int src_row0 = 0) {
	int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
	if (bw <= 0 || bh <= 0 || b.x0 < 0 || b.y0 < 0 || b.x1 > src.w || b.y1 > src.h || rows <= 0 || W <= 0) return false;
	for (int r = 0; r != rows; ++r) {
		const uint32_t* s = &src.px[(size_t)(b.y0 + mirror_index(src_row0 + r, bh)) * src.w + b.x0];
		uint32_t* d = &dst[(size_t)(y0 + r) * W];
		for (int x = 0; x != W; ++x) d[x] = s[mirror_index(x, bw)];
	}
	return true;
}

struct spec {
	box rail;  // the top `top` rows: a rail (its height is not required to match: rows are reflected)
	box body;  // the body and the lip
	int body_gain = 256;  // darkens the body (0..256) so text on it stays legible
};

// A W x H strip (the rail and the body may come from different images): `top` rows of rail, `lip` rows at the bottom (the body, darkened toward the
// edge), the body between. False (strip untouched) if a region is missing.
template <class Img>
bool build(const Img& rail_src, const Img& body_src, const spec& s, int W, int H, int top, int lip, Img& out) {
	if (W <= 0 || H <= top + lip || top < 0 || lip < 0) return false;
	std::vector<uint32_t> px((size_t)W * H, 0);
	if (!reflect_fill(rail_src, s.rail, px, W, 0, top)) return false;
	if (!reflect_fill(body_src, s.body, px, W, top, H - top)) return false;
	if (s.body_gain < 256)
		for (size_t i = (size_t)top * W; i != px.size(); ++i) px[i] = shade(px[i], std::max(0, s.body_gain));
	for (int r = 0; r != lip; ++r) {
		int gain = 256 - 160 * (r + 1) / lip;  // darkest on the last row
		for (int x = 0; x != W; ++x) { uint32_t& p = px[(size_t)(H - lip + r) * W + x]; p = shade(p, gain); }
	}
	out.w = W;
	out.h = H;
	out.px = std::move(px);
	return true;
}

// Cap columns: copies [0, cap) of `strip` into `out` and shades the outer `edge` columns
// (left: x = 0 side, right: the far side) so the panel reads as closed.
template <class Img>
void cap(const Img& strip, int cap_w, bool left, int edge, Img& out) {
	out.w = cap_w;
	out.h = strip.h;
	out.px.assign((size_t)cap_w * strip.h, 0);
	for (int y = 0; y != strip.h; ++y)
		for (int x = 0; x != cap_w; ++x) {
			int from = left ? x : cap_w - 1 - x;  // distance from the outer edge
			uint32_t p = strip.px[(size_t)y * strip.w + x];
			if (from < edge) p = shade(p, 96 + 160 * from / std::max(1, edge));
			out.px[(size_t)y * cap_w + x] = p;
		}
}

}  // namespace skin_strip

#endif
