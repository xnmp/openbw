#ifndef OPENBW_TT_FONT_H
#define OPENBW_TT_FONT_H

// TrueType text for the menus, which the game sets in Eurostile (font/eurostile-reg.ttf in a
// Remastered install; the font is the game's, read at run time and never shipped here).
// A thin layer over stb_truetype (ui/third_party/stb_truetype.h, public domain / MIT):
// glyphs are rendered once per pixel size and kept, and text is measured and drawn through
// a callback that receives each pixel's coverage, so it knows nothing about surfaces.

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

namespace tt_font {

// The code points of UTF-8 text; a malformed byte stands for itself (Latin-1), so player
// names in either encoding come out readable.
inline std::vector<int> code_points(const std::string& s) {
	std::vector<int> out;
	for (size_t i = 0; i < s.size();) {
		unsigned c = (unsigned char)s[i];
		// n: continuation bytes a lead byte announces (0: ASCII, a stray byte or invalid lead)
		int n = c >= 0xf8 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
		bool ok = n > 0 && i + n < s.size();
		unsigned v = n == 3 ? c & 7 : n == 2 ? c & 15 : c & 31;
		for (int k = 1; ok && k <= n; ++k) {
			unsigned d = (unsigned char)s[i + k];
			if ((d & 0xc0) != 0x80) ok = false;
			else v = v << 6 | (d & 63);
		}
		if (n > 0 && ok) {
			out.push_back((int)v);
			i += n + 1;
		} else {
			out.push_back((int)c);
			++i;
		}
	}
	return out;
}

class face {
public:
	// Takes the font file's bytes. False (and empty) for anything that is not a font.
	bool load(std::vector<uint8_t> ttf) {
		*this = face();
		if (ttf.size() < 12) return false;
		int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
		if (offset < 0 || !tables_inside(ttf, (size_t)offset)) return false;
		data = std::move(ttf);
		if (!stbtt_InitFont(&info, data.data(), offset) || info.numGlyphs <= 0) {
			*this = face();
			return false;
		}
		ok = true;
		return true;
	}
	bool loaded() const { return ok; }

	// Text width in pixels at a pixel height (the font's ascent to descent).
	double width(const std::string& text, int px) const {
		if (!ok || px <= 0) return 0;
		double s = stbtt_ScaleForPixelHeight(&info, (float)px), x = 0;
		int prev = 0;
		for (int cp : code_points(text)) {
			int gi = stbtt_FindGlyphIndex(&info, cp);
			if (prev) x += s * stbtt_GetGlyphKernAdvance(&info, prev, gi);
			int adv = 0, lsb = 0;
			stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
			x += s * adv;
			prev = gi;
		}
		return x;
	}

	// The largest pixel height at which `text` is at most `w` pixels wide (at least 1).
	int fit(const std::string& text, double w, int max_px = 200) const {
		int lo = 1, hi = std::max(1, max_px);
		while (lo < hi) {
			int mid = (lo + hi + 1) / 2;
			if (width(text, mid) <= w) lo = mid;
			else hi = mid - 1;
		}
		return lo;
	}

	// Ascent above the baseline and descent below it, in pixels at `px`.
	int ascent(int px) const { return metric(px, true); }
	int descent(int px) const { return metric(px, false); }
	// The height of a capital (an H) above the baseline: what text is centred on.
	int cap_height(int px) const {
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		if (!ok || px <= 0 || !stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1)) return ascent(px) * 7 / 10;
		return (int)std::lround(y1 * stbtt_ScaleForPixelHeight(&info, (float)px));
	}

	// Draws `text` with its left end at x and its baseline at y, calling
	// plot(px, py, index, coverage) for each covered pixel, where `index` is the position of
	// the code point in the text (so callers can colour single letters).
	template <class Plot>
	void draw(const std::string& text, int px, double x, int y, Plot&& plot) {
		if (!ok || px <= 0) return;
		double s = stbtt_ScaleForPixelHeight(&info, (float)px);
		int prev = 0, index = 0;
		for (int cp : code_points(text)) {
			int gi = stbtt_FindGlyphIndex(&info, cp);
			if (prev) x += s * stbtt_GetGlyphKernAdvance(&info, prev, gi);
			const glyph& g = rendered(gi, px, s);
			int gx = (int)std::lround(x) + g.x0, gy = y + g.y0;
			for (int r = 0; r != g.h; ++r)
				for (int c = 0; c != g.w; ++c) {
					int cov = g.cov[(size_t)r * g.w + c];
					if (cov) plot(gx + c, gy + r, index, cov);
				}
			x += s * g.advance;
			prev = gi;
			++index;
		}
	}

private:
	// stb_truetype trusts the file: it reads the table directory and the tables it names
	// without bounds checks. Checking that the directory and every table lie inside the
	// bytes turns a truncated or mislabelled file into a refusal. (Damage inside a table is
	// not checked; the font comes from the game's own store.)
	static bool tables_inside(const std::vector<uint8_t>& b, size_t off) {
		auto u16 = [&](size_t p) { return (size_t)b[p] << 8 | b[p + 1]; };
		auto u32 = [&](size_t p) { return (size_t)b[p] << 24 | (size_t)b[p + 1] << 16 | (size_t)b[p + 2] << 8 | b[p + 3]; };
		if (off > b.size() || b.size() - off < 12) return false;
		size_t n = u16(off + 4);
		if (n == 0 || (b.size() - off - 12) / 16 < n) return false;
		for (size_t i = 0; i != n; ++i) {
			size_t rec = off + 12 + 16 * i, at = u32(rec + 8), len = u32(rec + 12);
			if (at > b.size() || len > b.size() - at) return false;
		}
		return true;
	}

	struct glyph { int x0 = 0, y0 = 0, w = 0, h = 0, advance = 0; std::vector<uint8_t> cov; };

	const glyph& rendered(int gi, int px, double s) {
		auto key = std::make_pair(px, gi);
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		glyph g;
		int lsb = 0;
		stbtt_GetGlyphHMetrics(&info, gi, &g.advance, &lsb);
		int x1 = 0, y1 = 0;
		stbtt_GetGlyphBitmapBox(&info, gi, (float)s, (float)s, &g.x0, &g.y0, &x1, &y1);
		g.w = std::max(0, x1 - g.x0);
		g.h = std::max(0, y1 - g.y0);
		if (g.w > 4 * px + 8 || g.h > 4 * px + 8) g.w = g.h = 0;  // a broken glyph: draw nothing
		g.cov.assign((size_t)g.w * g.h, 0);
		if (g.w && g.h) stbtt_MakeGlyphBitmap(&info, g.cov.data(), g.w, g.h, g.w, (float)s, (float)s, gi);
		return cache.emplace(key, std::move(g)).first->second;
	}

	int metric(int px, bool up) const {
		if (!ok || px <= 0) return 0;
		int a = 0, d = 0, gap = 0;
		stbtt_GetFontVMetrics(&info, &a, &d, &gap);
		double s = stbtt_ScaleForPixelHeight(&info, (float)px);
		return (int)std::lround(up ? a * s : -d * s);
	}

	std::vector<uint8_t> data;
	stbtt_fontinfo info{};
	bool ok = false;
	std::map<std::pair<int, int>, glyph> cache;
};

}  // namespace tt_font

#endif
