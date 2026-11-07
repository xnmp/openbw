// Behaviour tests for the TrueType text layer (ui/tt_font.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/tt_font_test.cpp -o /tmp/tt_font_test && /tmp/tt_font_test [font.ttf]
// The drawing checks need a TrueType font: the one given, or a common system font; without
// one they are skipped (the decoding and rejection checks always run).
#include "tt_font.h"

#include <cstdio>
#include <fstream>
#include <iterator>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<uint8_t> read_file(const char* path) {
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
	using tt_font::code_points;
	// UTF-8 decodes; malformed bytes stand for themselves (Latin-1) rather than vanishing.
	CHECK(code_points("Ab") == (std::vector<int>{'A', 'b'}));
	CHECK(code_points("\xc3\xa9") == (std::vector<int>{0xe9}));
	CHECK(code_points("\xe2\x82\xac!") == (std::vector<int>{0x20ac, '!'}));
	CHECK(code_points("\xf0\x9f\x98\x80") == (std::vector<int>{0x1f600}));
	CHECK(code_points("\xe9t\xe9") == (std::vector<int>{0xe9, 't', 0xe9}));  // Latin-1
	CHECK(code_points("\xc3") == (std::vector<int>{0xc3}));                  // truncated at the end
	CHECK(code_points("\xe2\x82") == (std::vector<int>{0xe2, 0x82}));
	CHECK(code_points("\xc3(") == (std::vector<int>{0xc3, '('}));            // bad continuation
	CHECK(code_points("\xff\xfe") == (std::vector<int>{0xff, 0xfe}));
	CHECK(code_points("").empty());

	// Anything that is not a font is refused, and the face stays unusable (no crash, no text).
	tt_font::face f;
	CHECK(!f.load({}));
	CHECK(!f.load(std::vector<uint8_t>(11, 0)));
	CHECK(!f.load(std::vector<uint8_t>(4096, 0x5a)));
	CHECK(!f.loaded() && f.width("text", 20) == 0 && f.ascent(20) == 0);
	int plotted = 0;
	f.draw("text", 20, 0, 20, [&](int, int, int, int) { ++plotted; });
	CHECK(plotted == 0);

	const char* candidates[] = {argc > 1 ? argv[1] : "", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/Library/Fonts/Arial.ttf", "C:/Windows/Fonts/arial.ttf"};
	std::vector<uint8_t> ttf;
	for (const char* c : candidates) if (*c && (ttf = read_file(c)).size() > 1000) break;
	if (ttf.size() <= 1000) {
		std::printf("tt_font: no TrueType font found; drawing checks skipped\n");
	} else {
		// A truncated file is refused or, if its header survives, at least draws without
		// reading out of bounds (ASan watches).
		for (size_t n : {(size_t)12, (size_t)64, (size_t)300, ttf.size() / 2, ttf.size() - 1}) {
			tt_font::face cut;
			bool loaded = cut.load(std::vector<uint8_t>(ttf.begin(), ttf.begin() + n));
			if (n <= ttf.size() / 2) CHECK(!loaded);  // tables run past the end
			if (loaded) cut.draw("Save Game", 12, 0, 12, [](int, int, int, int) {});
		}

		CHECK(f.load(ttf) && f.loaded());
		// Width grows with the text and the size; nothing is no width.
		CHECK(f.width("", 20) == 0);
		CHECK(f.width("Save Game", 20) > f.width("Save", 20) && f.width("Save", 20) > 0);
		CHECK(f.width("Save Game", 40) > 1.8 * f.width("Save Game", 20));
		CHECK(f.width("x", 0) == 0 && f.width("x", -3) == 0);
		// fit: the largest size whose text fits, so one more pixel would not.
		int px = f.fit("Save Game", 128);
		CHECK(f.width("Save Game", px) <= 128 && f.width("Save Game", px + 1) > 128);
		CHECK(f.fit("Save Game", 0) == 1 && f.fit("", 50, 30) == 30);
		CHECK(f.ascent(20) > 0 && f.descent(20) >= 0 && f.ascent(20) + f.descent(20) <= 22);
		CHECK(f.cap_height(40) > 40 / 3 && f.cap_height(40) <= f.ascent(40));

		// Drawing: every pixel lands within the measured box around the baseline, with valid
		// coverage, and the letter index walks the text in order.
		const std::string text = "Hg Ok";
		const int size = 24, x0 = 10, base = 40;
		int minx = 1 << 30, maxx = -1, miny = 1 << 30, maxy = -1, last_index = -1, n = 0;
		bool in_order = true, cov_ok = true;
		f.draw(text, size, x0, base, [&](int x, int y, int index, int cov) {
			minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
			if (index < last_index) in_order = false;
			last_index = index;
			if (cov <= 0 || cov > 255) cov_ok = false;
			++n;
		});
		CHECK(n > 50 && in_order && cov_ok && last_index == 4);
		CHECK(minx >= x0 - 2 && maxx <= x0 + f.width(text, size) + 2);
		CHECK(miny >= base - f.ascent(size) - 1 && maxy <= base + f.descent(size) + 1);
		CHECK(miny < base - size / 3 && maxy > base);  // 'H' rises well above, 'g' hangs below
		// Drawing twice gives the same pixels (glyphs come from the cache the second time).
		int n2 = 0;
		f.draw(text, size, x0, base, [&](int, int, int, int) { ++n2; });
		CHECK(n2 == n);
		// Names in UTF-8 and characters the font lacks draw without trouble.
		f.draw("Zerg\xc3\xa9 \xe4\xb8\xad", 18, 0, 20, [](int, int, int, int) {});
	}

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("tt_font: all passed\n");
	return 0;
}
