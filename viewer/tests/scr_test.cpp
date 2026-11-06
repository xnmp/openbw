// Behaviour tests for the remastered asset readers and raster operations (ui/scr_*.h).
// Build and run: cmake --build build --target openbw-scr-test && ./build/openbw-scr-test
#include "scr_render.h"

#include <array>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <set>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

using namespace scr;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void put16(bytes& b, size_t at, uint16_t v) { b[at] = v & 0xff; b[at + 1] = v >> 8; }
static void put32(bytes& b, size_t at, uint32_t v) { for (int i = 0; i != 4; ++i) b[at + i] = (v >> (8 * i)) & 0xff; }
static void push16(bytes& b, uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
static void push32(bytes& b, uint32_t v) { for (int i = 0; i != 4; ++i) b.push_back((v >> (8 * i)) & 0xff); }
static void push32be(bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xff); }

// A DDS file of `w` x `h` with the given fourcc and block payload.
static bytes dds(int w, int h, const char* fourcc, const bytes& blocks) {
	bytes b(128, 0);
	std::memcpy(b.data(), "DDS ", 4);
	put32(b, 12, h);
	put32(b, 16, w);
	std::memcpy(b.data() + 84, fourcc, 4);
	b.insert(b.end(), blocks.begin(), blocks.end());
	return b;
}

static bytes bc1_block(uint16_t c0, uint16_t c1, uint32_t indices) {
	bytes b;
	push16(b, c0);
	push16(b, c1);
	push32(b, indices);
	return b;
}

static void test_bc1() {
	// Index i in pixel i: 0 red, 1 blue, 2 two-thirds red, 3 two-thirds blue.
	auto file = dds(4, 4, "DXT1", bc1_block(0xf800, 0x001f, 0b11100100));
	image img;
	CHECK(decode_dds(file.data(), file.size(), img));
	CHECK(img.w == 4 && img.h == 4);
	CHECK(img.at(0, 0) == rgba(255, 0, 0, 255));
	CHECK(img.at(1, 0) == rgba(0, 0, 255, 255));
	CHECK(img.at(2, 0) == rgba(170, 0, 85, 255));
	CHECK(img.at(3, 0) == rgba(85, 0, 170, 255));
	// c0 <= c1 selects the 3-colour mode: index 3 is transparent.
	auto punch = dds(4, 4, "DXT1", bc1_block(0x001f, 0xf800, 0xffffffff));
	CHECK(decode_dds(punch.data(), punch.size(), img));
	CHECK(ch_a(img.at(0, 0)) == 0);
}

static void test_bc3_alpha_and_partial_blocks() {
	// 6x2 image: two blocks wide, cropped to the image size.
	bytes blocks;
	for (int k = 0; k != 2; ++k) {
		bytes alpha = {255, 0, 0, 0, 0, 0, 0, 0};  // a0 = 255, a1 = 0, all indices 0
		alpha[2] = 0b001000;  // pixel 0 -> 0, pixel 1 -> index 1 (a1 = 0)
		blocks.insert(blocks.end(), alpha.begin(), alpha.end());
		auto c = bc1_block(0xffff, 0x0000, 0);
		blocks.insert(blocks.end(), c.begin(), c.end());
	}
	auto file = dds(6, 2, "DXT5", blocks);
	image img;
	CHECK(decode_dds(file.data(), file.size(), img));
	CHECK(img.w == 6 && img.h == 2 && img.px.size() == 12);
	CHECK(img.at(0, 0) == rgba(255, 255, 255, 255));
	CHECK(ch_a(img.at(1, 0)) == 0);
	CHECK(ch_a(img.at(4, 0)) == 255 && ch_a(img.at(5, 0)) == 0);  // second block, same pattern
	CHECK(ch_a(img.at(5, 1)) == 255);
}

static void test_dds_region() {
	// 8x4 DXT1: left block red, right block blue.
	bytes blocks = bc1_block(0xf800, 0xf800, 0);
	auto blue = bc1_block(0x001f, 0x001f, 0);
	blocks.insert(blocks.end(), blue.begin(), blue.end());
	auto file = dds(8, 4, "DXT1", blocks);
	image img;
	// A rectangle across the block boundary takes each pixel from its own block.
	CHECK(decode_dds_region(file.data(), file.size(), 2, 1, 4, 2, img));
	CHECK(img.w == 4 && img.h == 2);
	CHECK(img.at(0, 0) == rgba(255, 0, 0, 255) && img.at(1, 1) == rgba(255, 0, 0, 255));
	CHECK(img.at(2, 0) == rgba(0, 0, 255, 255) && img.at(3, 1) == rgba(0, 0, 255, 255));
	// Parts outside the texture are transparent; a rectangle entirely outside is all transparent.
	CHECK(decode_dds_region(file.data(), file.size(), -1, 3, 3, 2, img));
	CHECK(img.at(0, 0) == 0 && img.at(1, 0) == rgba(255, 0, 0, 255) && img.at(1, 1) == 0);
	CHECK(decode_dds_region(file.data(), file.size(), 100, 100, 2, 2, img));
	CHECK(img.px.size() == 4 && img.at(0, 0) == 0 && img.at(1, 1) == 0);
	// Negative sizes and malformed files are rejected.
	CHECK(!decode_dds_region(file.data(), file.size(), 0, 0, -1, 2, img));
	CHECK(!decode_dds_region(file.data(), 100, 0, 0, 2, 2, img));
}

static void test_dds_rejects_malformed() {
	image img;
	auto ok = dds(4, 4, "DXT1", bc1_block(0, 0, 0));
	CHECK(!decode_dds(ok.data(), 100, img));  // shorter than the header
	auto truncated = dds(8, 8, "DXT1", bc1_block(0, 0, 0));  // needs 4 blocks
	CHECK(!decode_dds(truncated.data(), truncated.size(), img));
	auto other = dds(4, 4, "ATI2", bc1_block(0, 0, 0));
	CHECK(!decode_dds(other.data(), other.size(), img));
	auto huge = dds(1 << 20, 4, "DXT1", bc1_block(0, 0, 0));
	CHECK(!decode_dds(huge.data(), huge.size(), img));
	bytes junk(200, 0xab);
	CHECK(!decode_dds(junk.data(), junk.size(), img));
}

// An uncompressed 32-bit DDS with pixel-format flags and channel masks (r, g, b, a).
static bytes dds_uncompressed(int w, int h, uint32_t flags, std::array<uint32_t, 4> masks, const bytes& pixels) {
	bytes b = dds(w, h, "\0\0\0\0", pixels);
	put32(b, 80, flags);
	put32(b, 88, 32);
	for (int i = 0; i != 4; ++i) put32(b, 92 + 4 * i, masks[i]);
	return b;
}

static void test_dds_uncompressed() {
	const uint32_t rgb = 0x40, alpha = 0x1;
	// B, G, R, A byte order, 2 x 1 pixels.
	bytes px = {10, 20, 30, 40, 50, 60, 70, 255};
	auto bgra = dds_uncompressed(2, 1, rgb | alpha, {{0xff0000, 0xff00, 0xff, 0xff000000}}, px);
	image img;
	CHECK(decode_dds(bgra.data(), bgra.size(), img) && img.w == 2 && img.h == 1);
	CHECK(img.px[0] == rgba(30, 20, 10, 40) && img.px[1] == rgba(70, 60, 50, 255));
	// A region: one pixel in, one outside (transparent).
	CHECK(decode_dds_region(bgra.data(), bgra.size(), 1, 0, 2, 1, img) && img.px[0] == rgba(70, 60, 50, 255) && img.px[1] == 0);
	// No alpha channel: opaque.
	auto opaque = dds_uncompressed(2, 1, rgb, {{0xff, 0xff00, 0xff0000, 0}}, px);
	CHECK(decode_dds(opaque.data(), opaque.size(), img) && img.px[0] == rgba(10, 20, 30, 255));
	// Masks that are not whole bytes, a short payload and other bit depths are refused.
	CHECK(!decode_dds(dds_uncompressed(2, 1, rgb | alpha, {{0xf800, 0x7e0, 0x1f, 0}}, px).data(), 136, img));
	auto short_px = dds_uncompressed(2, 1, rgb | alpha, {{0xff, 0xff00, 0xff0000, 0xff000000}}, bytes(7, 0));
	CHECK(!decode_dds(short_px.data(), short_px.size(), img));
	auto bits24 = bgra;
	put32(bits24, 88, 24);
	CHECK(!decode_dds(bits24.data(), bits24.size(), img));
}

// An .anim with one entry, layers {diffuse, teamcolor}, and `frames` frames.
static bytes anim_file(int scale, int frames, bool truncate_frames = false) {
	bytes f(0x14c, 0);
	std::memcpy(f.data(), "ANIM", 4);
	f[4] = (uint8_t)scale;
	f[5] = 2;
	put16(f, 8, 2);
	put16(f, 10, 1);
	std::memcpy(&f[12], "diffuse", 7);
	std::memcpy(&f[12 + 32], "teamcolor", 9);
	size_t entry = f.size();
	f.resize(entry + 12 + 2 * 12);
	put16(f, entry, frames);
	put16(f, entry + 2, 0xffff);
	put16(f, entry + 4, 128);
	put16(f, entry + 6, 96);
	auto tex = dds(4, 4, "DXT1", bc1_block(0xffff, 0xffff, 0));
	size_t frames_at = f.size() + tex.size();
	put32(f, entry + 8, (uint32_t)frames_at);
	put32(f, entry + 12, (uint32_t)f.size());  // diffuse
	put32(f, entry + 16, (uint32_t)tex.size());
	f.insert(f.end(), tex.begin(), tex.end());
	for (int i = 0; i != (truncate_frames ? frames - 1 : frames); ++i) {
		bytes fr(16, 0);
		put16(fr, 0, 4 * i);
		put16(fr, 4, (uint16_t)(int16_t)(10 + i));
		put16(fr, 6, (uint16_t)(int16_t)-3);
		put16(fr, 8, 8);
		put16(fr, 10, 6);
		f.insert(f.end(), fr.begin(), fr.end());
	}
	return f;
}

static void test_anim() {
	anim_entry e;
	CHECK(parse_anim(anim_file(2, 3), e));
	CHECK(e.scale == 2 && e.w == 128 && e.h == 96);
	CHECK(e.frames.size() == 3);
	CHECK(e.frames[2].tex_x == 8 && e.frames[2].x == 12 && e.frames[2].y == -3);
	CHECK(e.diffuse.offset != 0 && e.teamcolor.offset == 0);
	anim_entry bad;
	CHECK(!parse_anim(anim_file(2, 3, true), bad));
	bytes junk(0x200, 0);
	CHECK(!parse_anim(junk, bad));
	auto two_entries = anim_file(2, 1);
	put16(two_entries, 10, 2);
	CHECK(!parse_anim(two_entries, bad));
}

static void test_sd_references() {
	bytes f(0x14c, 0);
	std::memcpy(f.data(), "ANIM", 4);
	f[4] = 1;
	put16(f, 10, 3);
	size_t table = f.size();
	f.resize(table + 3 * 4);
	auto entry = [&](uint16_t frames, uint16_t ref) {
		size_t at = f.size();
		f.resize(at + 12);
		put16(f, at, frames);
		put16(f, at + 2, ref);
		return (uint32_t)at;
	};
	put32(f, table, entry(5, 0xffff));
	put32(f, table + 4, entry(0, 0));  // image 1 reuses image 0
	put32(f, table + 8, 0xfffffff0);  // offset past the end: no reference, no crash
	auto refs = parse_sd_anim_references(f);
	CHECK(refs.size() == 3);
	CHECK(refs[0] == -1 && refs[1] == 0 && refs[2] == -1);
	CHECK(parse_sd_anim_references(bytes(10, 0)).empty());
}

static void test_dds_grp() {
	auto a = dds(4, 4, "DXT1", bc1_block(0xf800, 0xf800, 0));
	auto b = dds(8, 4, "DXT1", bytes(16, 0));
	bytes f(8, 0);
	put16(f, 4, 2);
	for (auto* d : {&a, &b}) {
		bytes h(12, 0);
		put16(h, 4, (uint16_t)detail::le32(d->data() + 16));
		put16(h, 6, 4);
		put32(h, 8, (uint32_t)d->size());
		f.insert(f.end(), h.begin(), h.end());
		f.insert(f.end(), d->begin(), d->end());
	}
	put32(f, 0, (uint32_t)f.size());
	dds_grp g;
	CHECK(parse_dds_grp(f, g));
	CHECK(g.frames.size() == 2 && g.frames[1].w == 8);
	image img;
	CHECK(g.decode(0, img) && img.at(0, 0) == rgba(255, 0, 0, 255));
	CHECK(!g.decode(2, img));
	// A truncated file is rejected and leaves nothing behind, even in a grp that held frames.
	bytes cut(f.begin(), f.end() - 5);
	CHECK(!parse_dds_grp(cut, g));
	CHECK(g.frames.empty() && !g.decode(0, img));
}

static void test_resample() {
	image src;
	src.w = 10;
	src.h = 8;
	src.px.assign(80, rgba(200, 100, 50, 255));
	// Opaque tiles keep their colour to the edges when renormalised...
	image tile = resample(src, 0, 0, 10, 8, 7, 5, true);
	bool all_same = true;
	for (auto p : tile.px) all_same &= p == rgba(200, 100, 50, 255);
	CHECK(tile.w == 7 && tile.h == 5 && all_same);
	// ...while sprites fade at their edges and stay solid inside.
	image sprite = resample(src, 0, 0, 10, 8, 20, 16, false);
	CHECK(ch_a(sprite.at(10, 8)) == 255);
	CHECK(ch_a(sprite.at(0, 0)) < 255 && ch_a(sprite.at(0, 0)) > 0);
	// A flat image stays flat at non-integer scales (no ripple from the filter's phase).
	image wide;
	wide.w = 64;
	wide.h = 1;
	wide.px.assign(64, rgba(120, 120, 120, 255));
	for (double k : {0.75, 0.6, 0.4, 1.5}) {
		int dw = (int)(64 * k);
		image r = resample(wide, 0, 0, 64, 1, dw, 1, true);
		bool flat = true;
		for (int x = 0; x != dw; ++x) flat &= r.at(x, 0) == rgba(120, 120, 120, 255);
		CHECK(flat);
		image t = resample(wide, 0, 0, 64, 1, dw, 1, false);
		for (int x = 2; x < dw - 2; ++x) flat &= t.at(x, 0) == rgba(120, 120, 120, 255);
		CHECK(flat);
	}
	// Degenerate sizes give an empty result rather than reading out of bounds.
	CHECK(resample(src, 0, 0, 0, 8, 5, 5, true).px.size() == 25);
	CHECK(resample(src, 0, 0, 10, 8, 0, 5, true).px.empty());
}

struct test_canvas {
	std::vector<uint32_t> px;
	std::vector<uint8_t> shadow;
	canvas c;
	test_canvas(int w, int h, uint32_t fill) : px((size_t)w * h, fill), shadow((size_t)w * h, 0) {
		c.px = px.data();
		c.pitch = w;
		c.w = w;
		c.h = h;
		c.shadow = shadow.data();
	}
};

static sprite_frame solid_frame(int w, int h, uint32_t p, uint8_t mask) {
	sprite_frame f;
	f.px.w = w;
	f.px.h = h;
	f.px.px.assign((size_t)w * h, p);
	if (mask) f.mask.assign((size_t)w * h, mask);
	return f;
}

// Scaling a straight-alpha image (e.g. the cursor) goes through premultiplied alpha and back.
static void test_unpremultiply() {
	image img;
	img.w = 4;
	img.h = 1;
	img.px = {rgba(200, 100, 50, 255), rgba(200, 100, 50, 128), rgba(255, 255, 255, 3), rgba(9, 9, 9, 0)};
	image round = img;
	premultiply(round);
	unpremultiply(round);
	CHECK(round.px[0] == img.px[0]);
	auto near = [](uint32_t a, uint32_t b, uint32_t tol) {
		auto d = [](uint32_t x, uint32_t y) { return x > y ? x - y : y - x; };
		return d(ch_r(a), ch_r(b)) <= tol && d(ch_g(a), ch_g(b)) <= tol && d(ch_b(a), ch_b(b)) <= tol && ch_a(a) == ch_a(b);
	};
	CHECK(near(round.px[1], img.px[1], 1));
	// Very low alpha loses precision but stays in range and keeps its alpha.
	CHECK(ch_a(round.px[2]) == 3 && ch_r(round.px[2]) <= 255);
	// Fully transparent pixels carry no colour.
	CHECK(round.px[3] == 0);
	// Premultiplied input whose colour exceeds alpha (as resampling can round to) is clamped.
	image over;
	over.w = 1;
	over.h = 1;
	over.px = {rgba(20, 10, 0, 10)};
	unpremultiply(over);
	CHECK(over.px[0] == rgba(255, 255, 0, 10));
}

static void test_team_colour() {
	test_canvas t(4, 1, rgba(0, 0, 0, 255));
	uint32_t team = rgba(255, 0, 0, 255);
	// Fully masked white takes the team colour; unmasked white stays white.
	draw_sprite(t.c, solid_frame(2, 1, rgba(255, 255, 255, 255), 255), 0, 0, false, blend::normal, team, 255);
	draw_sprite(t.c, solid_frame(2, 1, rgba(255, 255, 255, 255), 0), 2, 0, false, blend::normal, team, 255);
	CHECK(t.px[0] == rgba(255, 0, 0, 255));
	CHECK(t.px[3] == rgba(255, 255, 255, 255));
	// Fully transparent sprite pixels leave the canvas alone.
	test_canvas u(2, 1, rgba(9, 9, 9, 255));
	draw_sprite(u.c, solid_frame(2, 1, 0, 255), 0, 0, false, blend::normal, team, 255);
	CHECK(u.px[0] == rgba(9, 9, 9, 255));
	// Off-canvas and partly off-canvas draws are clipped.
	draw_sprite(u.c, solid_frame(3, 3, rgba(255, 255, 255, 255), 0), -2, -2, false, blend::normal, team, 255);
	CHECK(u.px[0] == rgba(255, 255, 255, 255) && u.px[1] == rgba(9, 9, 9, 255));
	draw_sprite(u.c, solid_frame(3, 3, rgba(1, 1, 1, 255), 0), 100, -100, false, blend::normal, team, 255);
	CHECK(u.px[0] == rgba(255, 255, 255, 255) && u.px[1] == rgba(9, 9, 9, 255));
}

static void test_flip() {
	test_canvas t(2, 1, rgba(0, 0, 0, 255));
	sprite_frame f;
	f.px.w = 2;
	f.px.h = 1;
	f.px.px = {rgba(255, 0, 0, 255), rgba(0, 255, 0, 255)};
	draw_sprite(t.c, f, 0, 0, true, blend::normal, 0, 255);
	CHECK(t.px[0] == rgba(0, 255, 0, 255) && t.px[1] == rgba(255, 0, 0, 255));
}

static void test_shadows() {
	test_canvas t(3, 1, rgba(200, 200, 200, 255));
	auto shadow = solid_frame(3, 1, rgba(0, 0, 0, 255), 0);
	// Overlapping shadows do not darken beyond half...
	draw_sprite(t.c, shadow, 0, 0, false, blend::shadow, 0, 255);
	draw_sprite(t.c, shadow, 0, 0, false, blend::shadow, 0, 255);
	// ...and a sprite drawn later covers the shadow under it.
	draw_sprite(t.c, solid_frame(1, 1, rgba(200, 200, 200, 255), 0), 2, 0, false, blend::normal, 0, 255);
	resolve_shadows(t.c);
	CHECK(ch_r(t.px[0]) >= 99 && ch_r(t.px[0]) <= 101);
	CHECK(t.px[2] == rgba(200, 200, 200, 255));
	CHECK(t.shadow[0] == 0);  // resolved and cleared
}

// A frame whose pixels vary (colour, partial alpha, team mask), so that blends depend on
// the order draws are applied in.
static sprite_frame pattern_frame(int w, int h, uint32_t seed, bool with_mask) {
	sprite_frame f;
	f.px.w = w;
	f.px.h = h;
	for (int i = 0; i != w * h; ++i) {
		uint32_t v = (uint32_t)i * 2654435761u + seed * 40503u;
		uint32_t a = (v >> 24) % 3 == 0 ? 255 : (v >> 16) & 0xff;
		// premultiplied: colour channels at most alpha
		f.px.px.push_back(rgba((v & 0xff) * a / 255, ((v >> 8) & 0xff) * a / 255, ((v >> 4) & 0xff) * a / 255, a));
		if (with_mask) f.mask.push_back((uint8_t)(v >> 12));
	}
	return f;
}

// Recording draws and replaying them in parallel bands gives exactly the pixels of drawing
// them one after another, overlaps, shadows and clipping included.
static void test_draw_list() {
	const int w = 97, h = 233;  // tall enough to be split into many bands
	const uint32_t bg = rgba(10, 20, 30, 255);
	test_canvas seq(w, h, bg), banded(w, h, bg), whole(w, h, bg);
	box seq_area, banded_area;
	seq.c.shadow_area = &seq_area;
	banded.c.shadow_area = &banded_area;  // `whole` resolves its entire canvas
	image tile;
	tile.w = 40;
	tile.h = 70;
	for (int i = 0; i != tile.w * tile.h; ++i) tile.px.push_back(rgba(i & 0xff, (i >> 3) & 0xff, 99, 255));
	std::vector<sprite_frame> frames;
	for (uint32_t k = 0; k != 6; ++k) frames.push_back(pattern_frame(23 + 9 * k, 31 + 17 * k, k, k % 2 == 0));
	draw_list list;
	auto both = [&](auto&& direct, auto&& record) {
		direct(seq.c);
		direct(whole.c);
		record(banded.c);
	};
	both([&](const canvas& c) { draw_opaque(c, tile, -5, 12); }, [&](const canvas& c) { list.opaque(c, tile, -5, 12); });
	both([&](const canvas& c) { draw_opaque(c, tile, 50, 150); }, [&](const canvas& c) { list.opaque(c, tile, 50, 150); });
	const blend modes[] = {blend::normal, blend::shadow, blend::brighten, blend::tinted, blend::normal, blend::shadow};
	int n = 0;
	for (int round = 0; round != 3; ++round) {
		for (size_t k = 0; k != frames.size(); ++k, ++n) {
			int x = (n * 37) % (w + 40) - 30, y = (n * 53) % (h + 60) - 40;
			bool flipped = n % 3 == 1;
			uint32_t team = rgba(200, 40 + n, 90, 255), alpha = n % 4 == 0 ? 140 : 255;
			auto& f = frames[k];
			both([&](const canvas& c) { draw_sprite(c, f, x, y, flipped, modes[k], team, alpha); },
			     [&](const canvas& c) { list.sprite(c, f, x, y, flipped, modes[k], team, alpha); });
		}
		auto& f = frames[1];
		auto& t = frames[4];
		both([&](const canvas& c) { draw_sprite_mapped(c, f, t, 30, 60 + round * 50, round == 1); },
		     [&](const canvas& c) { list.mapped(c, f, t, 30, 60 + round * 50, round == 1); });
	}
	both([&](const canvas& c) { draw_sprite(c, frames[2], 1000, 1000, false, blend::normal, 0, 255); },
	     [&](const canvas& c) { list.sprite(c, frames[2], 1000, 1000, false, blend::normal, 0, 255); });
	CHECK(banded.px == std::vector<uint32_t>(w * h, bg));  // nothing drawn until run()
	list.run(banded.c);
	CHECK(list.commands.empty());
	CHECK(banded.shadow == seq.shadow);
	resolve_shadows(seq.c);
	resolve_shadows(banded.c);
	resolve_shadows(whole.c);
	CHECK(banded.px == seq.px);
	CHECK(whole.px == seq.px);  // resolving only the shadowed area matches resolving everything
	CHECK(seq.shadow == std::vector<uint8_t>(w * h, 0) && banded.shadow == seq.shadow);
	CHECK(seq_area.empty() && banded_area.empty());
}

static void test_parallel() {
	for (size_t n : {0, 1, 5, 1000, 100000}) {
		std::unique_ptr<std::atomic<int>[]> hits(new std::atomic<int>[n == 0 ? 1 : n]());
		parallel_for(n, 1, [&](size_t i) { hits[i].fetch_add(1); });
		bool once = true;
		for (size_t i = 0; i != n; ++i) once &= hits[i].load() == 1;
		CHECK(once);
	}
	// Calls from inside items complete (they run serially on that thread).
	std::atomic<size_t> total{0};
	parallel_for(64, 1, [&](size_t) { parallel_for(64, 1, [&](size_t) { total.fetch_add(1); }); });
	CHECK(total.load() == 64 * 64);
	// Ranges are non-empty, disjoint and cover [0, n).
	for (size_t n : {0, 1, 7, 1000, 4097}) {
		std::mutex m;
		std::vector<std::pair<size_t, size_t>> ranges;
		parallel_ranges(n, 3, [&](size_t a, size_t b) {
			std::lock_guard<std::mutex> l(m);
			ranges.emplace_back(a, b);
		});
		std::sort(ranges.begin(), ranges.end());
		size_t next = 0;
		bool ok = true;
		for (auto& r : ranges) {
			ok &= r.first == next && r.second > r.first;
			next = r.second;
		}
		CHECK(ok && next == n);
	}
	// An item that throws: the call still waits for every other item, then rethrows.
	for (size_t bad : {(size_t)0, (size_t)777, (size_t)9999}) {
		std::atomic<size_t> ran{0};
		bool caught = false;
		try {
			parallel_for(10000, 1, [&](size_t i) {
				if (i == bad) throw std::runtime_error("item");
				ran.fetch_add(1);
			});
		} catch (const std::runtime_error&) {
			caught = true;
		}
		CHECK(caught && ran.load() == 9999);
	}
	// ...and the pool still runs later calls in parallel.
	CHECK(!in_parallel_for());
	std::mutex m;
	std::set<std::thread::id> threads;
	parallel_for(4000, 1, [&](size_t) {
		std::this_thread::sleep_for(std::chrono::microseconds(20));
		std::lock_guard<std::mutex> l(m);
		threads.insert(std::this_thread::get_id());
	});
	CHECK(max_worker_threads() == 1 || threads.size() > 1);
}

// A minimal CASC store on disk: .build.info -> build config -> encoding/root -> a file.
static void write_file(const std::string& path, const bytes& b) {
	FILE* f = std::fopen(path.c_str(), "wb");
	std::fwrite(b.data(), 1, b.size(), f);
	std::fclose(f);
}

static std::string hex(const bytes& k) {
	std::string s;
	char t[3];
	for (auto v : k) { std::snprintf(t, sizeof t, "%02x", v); s += t; }
	return s;
}

static bytes key(uint8_t seed) {
	bytes k(16);
	for (int i = 0; i != 16; ++i) k[i] = (uint8_t)(seed * 17 + i);
	return k;
}

static void test_casc() {
	char tmpl[] = "/tmp/scr-casc-test-XXXXXX";
	std::string root_dir = mkdtemp(tmpl);
	std::string data = root_dir + "/Data";
	mkdir(data.c_str(), 0755);
	mkdir((data + "/data").c_str(), 0755);
	mkdir((data + "/config").c_str(), 0755);

	bytes content = {'h', 'e', 'l', 'l', 'o'};
	bytes file_ckey = key(1), file_ekey = key(2), root_ckey = key(3), root_ekey = key(4), enc_ekey = key(5);
	bytes lying_ckey = key(6), lying_ekey = key(7), nested_ckey = key(10), nested_ekey = key(11);
	bytes deep_ckey = key(12), deep_ekey = key(13), many_ckey = key(14), many_ekey = key(15), short_ckey = key(16), short_ekey = key(17);
	std::string root_text = "Some/Dir/File.txt|" + hex(file_ckey) + "\r\nbroken line\n" +
	                        "Lying.txt|" + hex(lying_ckey) + "\nNested.txt|" + hex(nested_ckey) + "\nDeep.txt|" + hex(deep_ckey) + "\n" +
	                        "Many.txt|" + hex(many_ckey) + "\nShort.txt|" + hex(short_ckey) + "\n";

	// BLTE: the file zlib-compressed in one chunk, the others as plain 'N' blobs.
	auto blte_plain = [](const bytes& payload) {
		bytes b = {'B', 'L', 'T', 'E', 0, 0, 0, 0, 'N'};
		b.insert(b.end(), payload.begin(), payload.end());
		return b;
	};
	// `declared` is the decoded size the chunk table claims (normally the payload's).
	auto blte_zlib = [](const bytes& payload, uint32_t declared) {
		uLongf n = compressBound(payload.size());
		bytes z(n);
		compress(z.data(), &n, payload.data(), payload.size());
		z.resize(n);
		bytes b = {'B', 'L', 'T', 'E'};
		push32be(b, 12 + 24);
		b.push_back(0x0f);
		b.push_back(0);
		b.push_back(0);
		b.push_back(1);
		push32be(b, (uint32_t)z.size() + 1);
		push32be(b, declared);
		b.resize(b.size() + 16);  // chunk md5 (unchecked)
		b.push_back('Z');
		b.insert(b.end(), z.begin(), z.end());
		return b;
	};
	// A chunk table of many chunks, alternately zlib ('Z') and plain ('N'), as large files are
	// stored (in 64 KB chunks).
	auto blte_chunks = [](const std::vector<bytes>& parts) {
		std::vector<bytes> chunks;
		for (size_t i = 0; i != parts.size(); ++i) {
			bytes c;
			if (i % 2 == 0) {
				uLongf n = compressBound(parts[i].size());
				bytes z(n);
				compress(z.data(), &n, parts[i].data(), parts[i].size());
				c.push_back('Z');
				c.insert(c.end(), z.begin(), z.begin() + n);
			} else {
				c.push_back('N');
				c.insert(c.end(), parts[i].begin(), parts[i].end());
			}
			chunks.push_back(c);
		}
		bytes b = {'B', 'L', 'T', 'E'};
		push32be(b, 12 + 24 * (uint32_t)parts.size());
		b.push_back(0x0f);
		b.push_back((uint8_t)(parts.size() >> 16));
		b.push_back((uint8_t)(parts.size() >> 8));
		b.push_back((uint8_t)parts.size());
		for (size_t i = 0; i != parts.size(); ++i) {
			push32be(b, (uint32_t)chunks[i].size());
			push32be(b, (uint32_t)parts[i].size());
			b.resize(b.size() + 16);
		}
		for (auto& c : chunks) b.insert(b.end(), c.begin(), c.end());
		return b;
	};
	std::vector<bytes> many_parts;
	bytes many_content;
	for (int i = 0; i != 100; ++i) {
		bytes part((size_t)(i * 37) % 300 + 1);
		for (size_t k = 0; k != part.size(); ++k) part[k] = (uint8_t)(i * 7 + k * 3);
		many_parts.push_back(part);
		many_content.insert(many_content.end(), part.begin(), part.end());
	}
	// A BLTE stream wrapped in `levels` nested 'F' (frame) chunks.
	auto blte_nested = [&](const bytes& inner, int levels) {
		bytes b = inner;
		for (int i = 0; i != levels; ++i) {
			bytes w = {'B', 'L', 'T', 'E', 0, 0, 0, 0, 'F'};
			w.insert(w.end(), b.begin(), b.end());
			b = w;
		}
		return b;
	};
	bytes encoding = {'E', 'N', 1, 16, 16};
	encoding.push_back(0); encoding.push_back(1);  // CE page size: 1 KB
	encoding.push_back(0); encoding.push_back(1);
	push32be(encoding, 1);  // CE page count
	push32be(encoding, 0);
	encoding.push_back(0);
	push32be(encoding, 0);  // espec size
	encoding.resize(encoding.size() + 32);  // page table
	size_t page = encoding.size();
	for (auto& pair : {std::make_pair(file_ckey, file_ekey), std::make_pair(root_ckey, root_ekey), std::make_pair(lying_ckey, lying_ekey),
	                   std::make_pair(nested_ckey, nested_ekey), std::make_pair(deep_ckey, deep_ekey), std::make_pair(many_ckey, many_ekey),
	                   std::make_pair(short_ckey, short_ekey)}) {
		encoding.push_back(1);
		encoding.resize(encoding.size() + 5);
		encoding.insert(encoding.end(), pair.first.begin(), pair.first.end());
		encoding.insert(encoding.end(), pair.second.begin(), pair.second.end());
	}
	encoding.resize(page + 1024);

	bytes archive, index_entries;
	auto add_blob = [&](const bytes& ekey, const bytes& blte) {
		uint64_t offset = archive.size();
		archive.resize(archive.size() + 30);
		archive.insert(archive.end(), blte.begin(), blte.end());
		index_entries.insert(index_entries.end(), ekey.begin(), ekey.begin() + 9);
		uint64_t so = offset;  // archive 0
		for (int i = 4; i >= 0; --i) index_entries.push_back((so >> (8 * i)) & 0xff);
		push32(index_entries, (uint32_t)(blte.size() + 30));
	};
	add_blob(file_ekey, blte_zlib(content, (uint32_t)content.size()));
	add_blob(lying_ekey, blte_zlib(content, 2));
	add_blob(nested_ekey, blte_nested(blte_plain(content), 1));
	add_blob(deep_ekey, blte_nested(blte_plain(content), 5));
	add_blob(many_ekey, blte_chunks(many_parts));
	add_blob(short_ekey, blte_zlib(content, (uint32_t)content.size() + 4));
	add_blob(root_ekey, blte_plain(bytes(root_text.begin(), root_text.end())));
	add_blob(enc_ekey, blte_plain(encoding));
	write_file(data + "/data/data.000", archive);

	bytes idx;
	push32(idx, 0x10);
	push32(idx, 0);
	push16(idx, 7);
	idx.push_back(0);
	idx.push_back(0);
	idx.push_back(4);
	idx.push_back(5);
	idx.push_back(9);
	idx.push_back(30);
	idx.resize(32, 0);
	push32(idx, (uint32_t)index_entries.size());
	push32(idx, 0);
	idx.insert(idx.end(), index_entries.begin(), index_entries.end());
	write_file(data + "/data/0000000001.idx", idx);

	std::string build_key = "0123456789abcdef0123456789abcdef";
	mkdir((data + "/config/01").c_str(), 0755);
	mkdir((data + "/config/01/23").c_str(), 0755);
	std::string cfg = "# Build Configuration\n\nroot = " + hex(root_ckey) + "\nencoding = " + hex(key(9)) + " " + hex(enc_ekey) + "\n";
	write_file(data + "/config/01/23/" + build_key, bytes(cfg.begin(), cfg.end()));
	std::string info = "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16\nus|1|" + build_key + "\n";
	write_file(root_dir + "/.build.info", bytes(info.begin(), info.end()));

	casc_storage c;
	std::string err;
	CHECK(c.open(root_dir, err));
	bytes out;
	CHECK(c.read("some/dir/FILE.TXT", out) && out == content);  // case-insensitive
	CHECK(c.read("Some\\Dir\\File.txt", out) && out == content);  // backslashes
	CHECK(c.has("Some/Dir/File.txt"));
	CHECK(!c.has("missing.txt") && !c.read("missing.txt", out));
	// A chunk that inflates past its declared size is rejected rather than trusted.
	CHECK(c.has("Lying.txt") && !c.read("Lying.txt", out));
	// Frame chunks nest a little; a deep chain of them is refused.
	CHECK(c.read("Nested.txt", out) && out == content);
	CHECK(c.has("Deep.txt") && !c.read("Deep.txt", out));
	// Many chunks (decoded in parallel) come back whole and in order...
	CHECK(c.read("Many.txt", out) && out == many_content);
	// ...and a chunk that decodes to less than its table size is rejected too.
	CHECK(c.has("Short.txt") && !c.read("Short.txt", out));

	casc_storage none;
	CHECK(!none.open(root_dir + "/nope", err) && !err.empty());
	std::string cmd = "rm -r '" + root_dir + "'";
	if (root_dir.rfind("/tmp/scr-casc-test-", 0) == 0) std::system(cmd.c_str());
}

// Against a real StarCraft: Remastered install, if one is given in $OPENBW_REMASTERED_DIR.
static void test_real_install() {
	const char* dir = std::getenv("OPENBW_REMASTERED_DIR");
	if (!dir || !*dir) {
		std::printf("skip real-install test (OPENBW_REMASTERED_DIR not set)\n");
		return;
	}
	graphics g;
	std::string err;
	CHECK(g.open(dir, err));
	if (!err.empty()) std::printf("  %s\n", err.c_str());
	CHECK(g.set_tileset("Jungle"));
	CHECK(g.megatile(1) && g.megatile(1)->w == g.tile_pixels());
	// Marine, Dragoon, Zealot (several facings) and the Command Center and Nexus (one frame
	// each), by images.dat id.
	const std::pair<size_t, size_t> cases[] = {{239, 0}, {239, 4}, {239, 13}, {122, 0}, {122, 4}, {122, 13},
	                                           {151, 0}, {151, 4}, {151, 13}, {275, 0}, {179, 0}};
	for (auto c : cases) {
		size_t id = c.first, frame = c.second;
		{
			g.set_zoom(2.0);  // HD2 atlas
			const sprite_frame* a = g.sprite(id, frame);
			CHECK(a && !a->px.empty());
			if (!a) {
				std::printf("  no HD2 frame %zu of image %zu\n", frame, id);
				continue;
			}
			sprite_frame hd2 = *a;
			// The per-image bounds used for culling contain this frame, flipped or not.
			const sprite_bounds* bb = g.bounds(id);
			CHECK(bb && bb->left <= hd2.left + 0.01 && hd2.left + hd2.px.w / 2.0 <= bb->right + 0.51);
			CHECK(bb && -bb->right <= hd2.left_flipped + 0.51 && bb->top <= hd2.top + 0.01 && hd2.top + hd2.px.h / 2.0 <= bb->bottom + 0.51);
			// Flipped, the frame covers the mirror image of its unflipped box about the
			// image centre.
			double w = hd2.px.w / 2.0;
			CHECK(std::abs(hd2.left_flipped + hd2.left + w) <= 1.0);
			g.set_zoom(3.0);  // full HD atlas: the same placement in game pixels
			const sprite_frame* b = g.sprite(id, frame);
			CHECK(b && !b->px.empty());
			if (!b) continue;
			CHECK(std::abs(b->left - hd2.left) <= 0.5 && std::abs(b->top - hd2.top) <= 0.5);
			CHECK(std::abs(b->px.w / 3.0 - hd2.px.w / 2.0) <= 1.0);
		}
	}
	// Images without art and ids past the table give null rather than failing.
	CHECK(g.sprite(5000, 0) == nullptr);
	CHECK(g.sprite(239, 100000) == nullptr);

	auto same = [](const sprite_frame* a, const sprite_frame* b) {
		return a && b && a->px.w == b->px.w && a->px.h == b->px.h && a->px.px == b->px.px && a->mask == b->mask &&
		       a->left == b->left && a->left_flipped == b->left_flipped && a->top == b->top;
	};
	// Zooming across 2x and back (HD2 -> HD -> HD2) gives the same art as before.
	g.set_zoom(1.5);
	sprite_frame before = *g.sprite(239, 4);
	const image* tile = g.megatile(1);
	image tile_before = tile ? *tile : image{};
	g.set_zoom(3.0);
	CHECK(g.sprite(239, 4) && g.megatile(1));
	g.set_zoom(1.5);
	CHECK(same(g.sprite(239, 4), &before));
	CHECK(g.megatile(1) && g.megatile(1)->px == tile_before.px);

	// Loading and scaling in parallel batches gives exactly what drawing one at a time does.
	graphics batch;
	CHECK(batch.open(dir, err) && batch.set_tileset("Jungle"));
	batch.set_zoom(1.5);
	std::vector<size_t> ids;
	std::vector<std::pair<size_t, size_t>> wanted;
	for (auto c : cases) {
		ids.push_back(c.first);
		wanted.push_back(c);
	}
	wanted.push_back({5000, 0});  // no art: ignored
	batch.load(ids);
	batch.prepare(wanted);
	std::vector<size_t> tiles = {1, 2, 3, 100, 2, 999999};
	batch.prepare_megatiles(tiles);
	for (auto c : cases) CHECK(same(batch.sprite(c.first, c.second), g.sprite(c.first, c.second)));
	for (size_t t : {1, 2, 3, 100}) CHECK(batch.megatile(t) && g.megatile(t) && batch.megatile(t)->px == g.megatile(t)->px);
	CHECK(batch.megatile(999999) == nullptr);

	// Budgets: over them, end_frame() drops only what the frame just drawn did not use, so a
	// screen needing more than the budget is kept rather than rebuilt every frame.
	graphics lru;
	CHECK(lru.open(dir, err) && lru.set_tileset("Jungle"));
	lru.set_zoom(1.5);
	lru.source_budget = 1;
	lru.scaled_budget = 1;
	// Frame 1 draws a Marine and a Dragoon frame and a megatile: all kept.
	CHECK(lru.sprite(239, 0) && lru.sprite(122, 0) && lru.megatile(1));
	size_t source1 = lru.held_source_bytes(), scaled1 = lru.held_scaled_bytes();
	lru.end_frame();
	CHECK(lru.held_source_bytes() == source1 && lru.held_scaled_bytes() == scaled1);
	// Frame 2 draws only the Marine: the Dragoon's atlas and frame and the megatile go.
	sprite_frame marine = *lru.sprite(239, 0);
	lru.end_frame();
	CHECK(lru.held_source_bytes() < source1 && lru.held_source_bytes() > 0);
	CHECK(lru.held_scaled_bytes() == marine.px.px.size() * 4 + marine.mask.size());
	// What was dropped comes back the same when drawn again.
	CHECK(same(lru.sprite(122, 0), g.sprite(122, 0)));
	CHECK(lru.megatile(1) && g.megatile(1) && lru.megatile(1)->px == g.megatile(1)->px);
	CHECK(same(lru.sprite(239, 0), &marine));

	// The least recently used goes first, and only down to 3/4 of the budget: drawn in turn
	// over three frames, a Dragoon, a Marine and a Zealot frame, with room for the last two.
	graphics order;
	CHECK(order.open(dir, err) && order.set_tileset("Jungle"));
	order.set_zoom(1.5);
	auto bytes_of = [](const sprite_frame* f) { return f ? f->px.px.size() * 4 + f->mask.size() : 0; };
	size_t dragoon = bytes_of(order.sprite(122, 0));
	order.end_frame();
	size_t marine_bytes = bytes_of(order.sprite(239, 0));
	order.end_frame();
	size_t zealot = bytes_of(order.sprite(151, 0));
	CHECK(dragoon && marine_bytes && zealot && dragoon != marine_bytes);
	order.scaled_budget = (marine_bytes + zealot) * 4 / 3 + 4;
	CHECK(order.held_scaled_bytes() > order.scaled_budget);
	order.end_frame();
	CHECK(order.held_scaled_bytes() == marine_bytes + zealot);
}

int main() {
	test_bc1();
	test_bc3_alpha_and_partial_blocks();
	test_dds_region();
	test_dds_rejects_malformed();
	test_dds_uncompressed();
	test_anim();
	test_sd_references();
	test_dds_grp();
	test_resample();
	test_unpremultiply();
	test_team_colour();
	test_flip();
	test_shadows();
	test_draw_list();
	test_parallel();
	test_casc();
	test_real_install();
	if (failures) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("all scr tests passed\n");
	return 0;
}
