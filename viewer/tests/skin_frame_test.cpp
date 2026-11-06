// Behaviour tests for the race panel frame builder (ui/skin_frame.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/skin_frame_test.cpp -o /tmp/skin_frame_test && /tmp/skin_frame_test
#include "skin_frame.h"

#include <cstdio>

using namespace skin_frame;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

struct img { int w = 0, h = 0; std::vector<uint32_t> px; };

// A source whose pixel at (x, y) encodes its own position (opaque), so which source pixel
// landed where is observable.
static uint32_t code(int x, int y) { return (uint32_t)x | (uint32_t)y << 8 | 0xff000000u; }
static img coded(int w, int h) {
	img s; s.w = w; s.h = h;
	for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) s.px.push_back(code(x, y));
	return s;
}
static uint32_t at(const img& i, int x, int y) { return i.px[(size_t)y * i.w + x]; }

int main() {
	img src = coded(40, 30), out;
	spec s;
	s.x = 2; s.y = 3; s.corner = 4;
	s.top_from = 10; s.top_to = 13;  // 3 columns
	s.left_from = 20; s.left_to = 22;  // 2 rows
	layout l;
	CHECK(build(src, s, out, l));
	CHECK(l.corner == 4 && l.edge_x == 6 && l.edge_y == 4);
	CHECK(out.w == l.w() && out.h == l.h() && out.w == 14 && out.h == 12 && out.px.size() == 168u);

	// The top-left corner is the source corner as is.
	CHECK(at(out, 0, 0) == code(2, 3) && at(out, 3, 3) == code(5, 6) && at(out, 1, 2) == code(3, 5));
	// The other corners are its mirror images.
	CHECK(at(out, 13, 0) == code(2, 3) && at(out, 10, 0) == code(5, 3));
	CHECK(at(out, 0, 11) == code(2, 3) && at(out, 0, 8) == code(2, 6));
	CHECK(at(out, 13, 11) == code(2, 3) && at(out, 10, 8) == code(5, 6));

	// The top edge: the run forward then backward, over the corner's rows; the bottom edge
	// is it upside down.
	for (int k = 0; k != 6; ++k) {
		int sx = 10 + (k < 3 ? k : 5 - k);
		CHECK(at(out, 4 + k, 0) == code(sx, 3));
		CHECK(at(out, 4 + k, 2) == code(sx, 5));
		CHECK(at(out, 4 + k, 11) == code(sx, 3));
	}
	// Repeating the edge end to end has no seam: its two ends are the same column.
	CHECK(at(out, 4, 1) == at(out, 9, 1));
	// The left edge: the run down then up, over the corner's columns; the right edge mirrored.
	for (int k = 0; k != 4; ++k) {
		int sy = 20 + (k < 2 ? k : 3 - k);
		CHECK(at(out, 0, 4 + k) == code(2, sy));
		CHECK(at(out, 3, 4 + k) == code(5, sy));
		CHECK(at(out, 13, 4 + k) == code(2, sy));
	}
	// The centre is transparent.
	for (int y = 4; y != 8; ++y) for (int x = 4; x != 10; ++x) CHECK(at(out, x, y) == 0);

	// Regions outside the source are refused, leaving the output alone.
	img keep = out;
	layout keep_l = l;
	spec bad = s; bad.top_to = 41;
	CHECK(!build(src, bad, out, l));
	bad = s; bad.corner = 0;
	CHECK(!build(src, bad, out, l));
	bad = s; bad.y = 27;  // the corner would run off the bottom
	CHECK(!build(src, bad, out, l));
	bad = s; bad.left_from = 22;  // an empty run
	CHECK(!build(src, bad, out, l));
	bad = s; bad.x = -1;
	CHECK(!build(src, bad, out, l));
	CHECK(out.px == keep.px && l.w() == keep_l.w() && l.h() == keep_l.h());
	CHECK(!build(img{}, s, out, l));

	// Scaling rounds positions, keeps runs non-empty and rounds insets up (so text never
	// overlaps a frame that grew by rounding).
	spec big; big.x = 0; big.y = 48; big.corner = 32; big.top_from = 40; big.top_to = 41;
	big.left_from = 100; big.left_to = 140; big.inset_top = 17; big.inset_side = 14;
	spec h = scaled(big, 0.5);
	CHECK(h.y == 24 && h.corner == 16 && h.left_from == 50 && h.left_to == 70);
	CHECK(h.top_to > h.top_from);
	CHECK(h.inset_top == 9 && h.inset_side == 7);
	CHECK(scaled(big, 0.01).corner >= 1);

	// Opening the face: a frame of bright rim around dark screen pixels (and a dark pixel
	// outside the rim) — only the dark pixels connected to the centre are cleared.
	{
		const uint32_t rim = 0xff8080c0u, dark = 0xff080808u;
		img f; f.w = 14; f.h = 12; f.px.assign(168, dark);
		layout fl{4, 6, 4};
		for (int y = 0; y != 12; ++y) for (int x = 0; x != 14; ++x) {
			if (x == 1 || y == 1 || x == 12 || y == 10) f.px[(size_t)y * 14 + x] = rim;
			if (x >= 4 && x < 10 && y >= 4 && y < 8) f.px[(size_t)y * 14 + x] = 0;  // the centre
		}
		open_face(f, fl, 24);
		CHECK(at(f, 2, 2) == 0 && at(f, 11, 9) == 0 && at(f, 3, 6) == 0);  // inside the rim: cleared
		CHECK(at(f, 1, 5) == rim && at(f, 6, 1) == rim);  // the rim stays
		CHECK(at(f, 0, 0) == dark && at(f, 13, 11) == dark);  // outside the rim: not reached
		img wrong = f; layout bad{5, 6, 4};
		open_face(wrong, bad, 24);  // a layout that does not match the image: untouched
		CHECK(wrong.px == f.px);
	}

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("skin_frame: all passed\n");
	return 0;
}
