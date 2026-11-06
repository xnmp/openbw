// Behaviour tests for the race panel strip builder (ui/skin_strip.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/skin_strip_test.cpp -o /tmp/skin_strip_test && /tmp/skin_strip_test
#include "skin_strip.h"

#include <cstdio>

using namespace skin_strip;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

struct img { int w = 0, h = 0; std::vector<uint32_t> px; };

static uint32_t pix(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

// A source whose pixel at (x, y) encodes its own position, so reflection is observable.
static img coded(int w, int h) {
	img s; s.w = w; s.h = h;
	for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) s.px.push_back(pix(x, y, 0, 255));
	return s;
}

int main() {
	// Reflection bounces at both ends and never leaves 0..n-1.
	CHECK(mirror_index(0, 4) == 0 && mirror_index(3, 4) == 3);
	CHECK(mirror_index(4, 4) == 3 && mirror_index(7, 4) == 0 && mirror_index(8, 4) == 0 && mirror_index(11, 4) == 3);
	CHECK(mirror_index(5, 1) == 0 && mirror_index(5, 0) == 0);
	for (int i = 0; i != 100; ++i) { int m = mirror_index(i, 7); CHECK(m >= 0 && m < 7); }

	// Shading keeps alpha, and never brightens a premultiplied pixel.
	uint32_t p = pix(200, 100, 50, 255), q = shade(p, 128);
	CHECK((q >> 24) == 255 && (q & 255) == 100 && ((q >> 8) & 255) == 50 && ((q >> 16) & 255) == 25);
	CHECK(shade(p, 256) == p && shade(p, 0) == pix(0, 0, 0, 255));

	// A strip: rail rows come from the rail region, body rows from the body region, and the
	// last row is darker than the body.
	img src = coded(8, 8), out;
	spec s{{0, 0, 4, 2}, {4, 4, 8, 8}};
	CHECK(build(src, src, s, 10, 12, 3, 2, out));
	CHECK(out.w == 10 && out.h == 12 && out.px.size() == 120);
	CHECK((out.px[0] & 255) == 0 && ((out.px[0] >> 8) & 255) == 0);               // rail (0,0) <- src (0,0)
	CHECK((out.px[4] & 255) == 3);                                                 // x = 4 reflects back to src x 3
	CHECK(((out.px[(size_t)1 * 10] >> 8) & 255) == 1);                            // rail row 1
	CHECK(((out.px[(size_t)2 * 10] >> 8) & 255) == 1);                            // row 2 bounces back onto rail row 1 (rail has 2 rows)
	CHECK((out.px[(size_t)3 * 10] & 255) == 4 && ((out.px[(size_t)3 * 10] >> 8) & 255) == 4);  // body (0,0) <- src (4,4)
	CHECK((out.px[(size_t)11 * 10] & 255) < 4);                                    // lip row is shaded
	CHECK((out.px[(size_t)11 * 10] >> 24) == 255);

	// body_gain darkens the body but not the rail.
	img dim;
	spec sd = s; sd.body_gain = 128;
	CHECK(build(src, src, sd, 10, 12, 3, 2, dim));
	CHECK(dim.px[0] == out.px[0] && (dim.px[(size_t)3 * 10] & 255) == 2);  // body x=4 -> 4 * 128 / 256

	// Malformed input is refused and leaves the output alone.
	img keep = out;
	CHECK(!build(src, src, spec{{0, 0, 0, 2}, {4, 4, 8, 8}}, 10, 12, 3, 2, keep));        // empty rail
	CHECK(!build(src, src, spec{{0, 0, 4, 2}, {4, 4, 9, 8}}, 10, 12, 3, 2, keep));        // past the source
	CHECK(!build(src, src, spec{{-1, 0, 4, 2}, {4, 4, 8, 8}}, 10, 12, 3, 2, keep));       // negative origin
	CHECK(!build(src, src, s, 0, 12, 3, 2, keep));                                         // no width
	CHECK(!build(src, src, s, 10, 4, 3, 2, keep));                                         // no room for body
	CHECK(keep.px == out.px && keep.w == out.w);
	img empty;
	CHECK(!build(src, empty, s, 10, 12, 3, 2, keep));  // missing body image
	// The rail and the body can come from different images.
	img other = coded(8, 8);
	for (auto& q : other.px) q = pix(250, 0, 0, 255);
	CHECK(build(other, src, s, 10, 12, 3, 2, out) && (out.px[0] & 255) == 250 && (out.px[(size_t)3 * 10] & 255) == 4);
	CHECK(!build(empty, src, s, 10, 12, 3, 2, keep));

	// Caps shade their outer columns only.
	img strip; strip.w = 6; strip.h = 1; strip.px.assign(6, pix(200, 200, 200, 255));
	img lc, rc;
	cap(strip, 6, true, 2, lc);
	cap(strip, 6, false, 2, rc);
	CHECK((lc.px[0] & 255) < (lc.px[1] & 255) && (lc.px[1] & 255) < 200 && (lc.px[2] & 255) == 200);
	CHECK((rc.px[5] & 255) < (rc.px[4] & 255) && (rc.px[3] & 255) == 200 && (rc.px[0] & 255) == 200);

	std::printf(failures ? "%d FAILED\n" : "ok\n", failures);
	return failures != 0;
}
