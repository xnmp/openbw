#ifndef OPENBW_SCR_ASSETS_H
#define OPENBW_SCR_ASSETS_H

// StarCraft: Remastered asset formats, decoded for a software renderer.
//
//  * DDS textures: DXT1/BC1 and DXT5/BC3 (the HD art), and uncompressed 32-bit (cursors).
//  * .anim sprite files: HD ("anim/main_NNN.anim", 4 texels per game pixel) and HD2
//    ("HD2/anim/main_NNN.anim", 2 texels per game pixel), one per images.dat entry, with
//    a texture atlas per layer (diffuse, teamcolor, ...) and a frame table of atlas rects.
//    "SD/mainSD.anim" holds all 999 entries; an entry with no frames there references
//    another image's graphics, which the HD files then lack.
//  * .dds.grp / .dds.vr4 containers: a list of whole DDS frames (tileset megatiles, creep
//    edges).
//
// Pixels are 32-bit r | g << 8 | b << 16 | a << 24, the viewer's RGBA surface layout.
// Sprite pixels are stored with premultiplied alpha so that filtering does not darken edges.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace scr {

using bytes = std::vector<uint8_t>;

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
	return r | g << 8 | b << 16 | a << 24;
}
inline uint32_t ch_r(uint32_t p) { return p & 0xff; }
inline uint32_t ch_g(uint32_t p) { return (p >> 8) & 0xff; }
inline uint32_t ch_b(uint32_t p) { return (p >> 16) & 0xff; }
inline uint32_t ch_a(uint32_t p) { return p >> 24; }

struct image {
	int w = 0;
	int h = 0;
	std::vector<uint32_t> px;
	bool empty() const { return px.empty(); }
	uint32_t at(int x, int y) const { return px[(size_t)y * w + x]; }
};

namespace detail {

inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

inline void rgb565(uint16_t c, int& r, int& g, int& b) {
	r = (c >> 11) & 31;
	g = (c >> 5) & 63;
	b = c & 31;
	r = r << 3 | r >> 2;
	g = g << 2 | g >> 4;
	b = b << 3 | b >> 2;
}

// Colour part of a BC1/BC3 block into 16 pixels (alpha 255; BC1 punch-through gives 0).
inline void decode_color_block(const uint8_t* b, uint32_t out[16], bool allow_punch_through) {
	uint16_t c0 = le16(b), c1 = le16(b + 2);
	int r[4], g[4], bl[4];
	uint32_t a[4] = {255, 255, 255, 255};
	rgb565(c0, r[0], g[0], bl[0]);
	rgb565(c1, r[1], g[1], bl[1]);
	if (c0 > c1 || !allow_punch_through) {
		r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; bl[2] = (2 * bl[0] + bl[1]) / 3;
		r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; bl[3] = (bl[0] + 2 * bl[1]) / 3;
	} else {
		r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; bl[2] = (bl[0] + bl[1]) / 2;
		r[3] = g[3] = bl[3] = 0;
		a[3] = 0;
	}
	uint32_t bits = le32(b + 4);
	for (int i = 0; i != 16; ++i) {
		int k = (bits >> (2 * i)) & 3;
		out[i] = rgba(r[k], g[k], bl[k], a[k]);
	}
}

inline void decode_bc3_alpha(const uint8_t* b, uint8_t out[16]) {
	int a0 = b[0], a1 = b[1];
	int a[8];
	a[0] = a0;
	a[1] = a1;
	if (a0 > a1) {
		for (int i = 1; i != 7; ++i) a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
	} else {
		for (int i = 1; i != 5; ++i) a[i + 1] = ((5 - i) * a0 + i * a1) / 5;
		a[6] = 0;
		a[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i != 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
	for (int i = 0; i != 16; ++i) out[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
}

}

// Header of a DDS file the decoder supports (top mip level): DXT1, DXT5, or uncompressed
// 32-bit with one byte per channel (the remastered cursors).
struct dds_info {
	int w = 0, h = 0;
	bool bc1 = false;
	bool uncompressed = false;
	int shift[4] = {0, 8, 16, 24};  // uncompressed: bit position of r, g, b, a
	const uint8_t* blocks = nullptr;  // or pixels
	size_t block_bytes() const { return bc1 ? 8 : 16; }
};

inline bool dds_header(const uint8_t* data, size_t size, dds_info& info) {
	using namespace detail;
	if (size < 128 || std::memcmp(data, "DDS ", 4) != 0) return false;
	int h = (int)le32(data + 12), w = (int)le32(data + 16);
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
	const uint32_t rgb_flag = 0x40, alpha_flag = 0x1;
	uint32_t pf_flags = le32(data + 80), bits = le32(data + 88);
	const uint8_t* fourcc = data + 84;
	bool bc1 = std::memcmp(fourcc, "DXT1", 4) == 0;
	bool bc3 = std::memcmp(fourcc, "DXT5", 4) == 0;
	if (!bc1 && !bc3 && (pf_flags & rgb_flag) && bits == 32) {
		// Each channel mask must be one whole byte; without an alpha mask, alpha is opaque.
		int shift[4];
		for (int c = 0; c != 4; ++c) {
			uint32_t mask = le32(data + 92 + 4 * c);
			if (c == 3 && (!(pf_flags & alpha_flag) || mask == 0)) {
				shift[c] = -1;
				continue;
			}
			shift[c] = -1;
			for (int b = 0; b != 32; b += 8) if (mask == 0xffu << b) shift[c] = b;
			if (shift[c] < 0) return false;
		}
		if (128 + (size_t)w * h * 4 > size) return false;
		info.w = w;
		info.h = h;
		info.bc1 = false;
		info.uncompressed = true;
		std::copy(shift, shift + 4, info.shift);
		info.blocks = data + 128;
		return true;
	}
	if (!bc1 && !bc3) return false;
	size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	if (128 + bw * bh * (bc1 ? 8 : 16) > size) return false;
	info.w = w;
	info.h = h;
	info.bc1 = bc1;
	info.uncompressed = false;
	info.blocks = data + 128;
	return true;
}

// Decodes the `rw` x `rh` rectangle at (rx, ry) of a DDS file, touching only the blocks it
// covers; parts outside the texture are transparent. Atlases stay compressed in memory and
// frames are decoded from them on demand.
inline bool decode_dds_region(const uint8_t* data, size_t size, int rx, int ry, int rw, int rh, image& out) {
	using namespace detail;
	dds_info info;
	if (!dds_header(data, size, info) || rw < 0 || rh < 0) return false;
	out.w = rw;
	out.h = rh;
	out.px.assign((size_t)rw * rh, 0);
	int x0 = std::max(rx, 0), y0 = std::max(ry, 0);
	int x1 = std::min(rx + rw, info.w), y1 = std::min(ry + rh, info.h);
	if (x0 >= x1 || y0 >= y1) return true;
	if (info.uncompressed) {
		for (int y = y0; y != y1; ++y) {
			for (int x = x0; x != x1; ++x) {
				uint32_t v = le32(info.blocks + ((size_t)y * info.w + x) * 4);
				auto ch = [&](int c) { return info.shift[c] < 0 ? 255u : (v >> info.shift[c]) & 0xff; };
				out.px[(size_t)(y - ry) * rw + (x - rx)] = rgba(ch(0), ch(1), ch(2), ch(3));
			}
		}
		return true;
	}
	int blocks_w = (info.w + 3) / 4;
	uint32_t block[16];
	uint8_t alpha[16];
	for (int by = y0 / 4; by <= (y1 - 1) / 4; ++by) {
		for (int bx = x0 / 4; bx <= (x1 - 1) / 4; ++bx) {
			const uint8_t* p = info.blocks + ((size_t)by * blocks_w + bx) * info.block_bytes();
			if (info.bc1) {
				decode_color_block(p, block, true);
			} else {
				decode_bc3_alpha(p, alpha);
				decode_color_block(p + 8, block, false);
				for (int i = 0; i != 16; ++i) block[i] = (block[i] & 0xffffff) | (uint32_t)alpha[i] << 24;
			}
			for (int y = 0; y != 4; ++y) {
				int py = by * 4 + y;
				if (py < y0 || py >= y1) continue;
				for (int x = 0; x != 4; ++x) {
					int px = bx * 4 + x;
					if (px < x0 || px >= x1) continue;
					out.px[(size_t)(py - ry) * rw + (px - rx)] = block[y * 4 + x];
				}
			}
		}
	}
	return true;
}

// Decodes a whole DDS file (top mip level). Returns false for unsupported formats.
inline bool decode_dds(const uint8_t* data, size_t size, image& out) {
	dds_info info;
	return dds_header(data, size, info) && decode_dds_region(data, size, 0, 0, info.w, info.h, out);
}

inline void premultiply(image& img) {
	for (auto& p : img.px) {
		uint32_t a = ch_a(p);
		if (a == 255) continue;
		p = rgba(ch_r(p) * a / 255, ch_g(p) * a / 255, ch_b(p) * a / 255, a);
	}
}

// The inverse of premultiply(), up to rounding.
inline void unpremultiply(image& img) {
	for (auto& p : img.px) {
		uint32_t a = ch_a(p);
		if (a == 255) continue;
		if (a == 0) {
			p = 0;
			continue;
		}
		auto un = [a](uint32_t c) { return std::min<uint32_t>(255, (c * 255 + a / 2) / a); };
		p = rgba(un(ch_r(p)), un(ch_g(p)), un(ch_b(p)), a);
	}
}

// ---- .anim ----

// Frame and box coordinates are in HD units (4 per game pixel) in HD and HD2 files alike;
// an HD2 atlas is half that resolution.
struct anim_frame {
	int tex_x, tex_y;  // rect in the layer atlases
	int x, y;  // offset of the rect within the entry's w x h box
	int w, h;
};

struct anim_layer {
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct anim_entry {
	int scale = 0;  // texels per game pixel: 4 (HD), 2 (HD2), 1 (SD)
	int w = 0, h = 0;  // the box frames are positioned in, centred on the image position (HD units)
	std::vector<anim_frame> frames;
	anim_layer diffuse, teamcolor;
	int reference = -1;  // SD only: the image whose graphics this entry reuses
};

namespace detail {

inline bool parse_anim_entry(const bytes& f, size_t pos, int scale, const std::vector<std::string>& layer_names, anim_entry& e) {
	if (pos + 12 > f.size()) return false;
	e.scale = scale;
	size_t frame_count = le16(&f[pos]);
	if (frame_count == 0) {
		e.reference = le16(&f[pos + 2]);
		return true;
	}
	e.w = le16(&f[pos + 4]);
	e.h = le16(&f[pos + 6]);
	uint32_t frames_offset = le32(&f[pos + 8]);
	for (size_t i = 0; i != layer_names.size(); ++i) {
		size_t lp = pos + 12 + i * 12;
		if (lp + 12 > f.size()) return false;
		anim_layer l{le32(&f[lp]), le32(&f[lp + 4])};
		if (l.offset && (size_t)l.offset + l.size > f.size()) return false;
		if (layer_names[i] == "diffuse") e.diffuse = l;
		else if (layer_names[i] == "teamcolor") e.teamcolor = l;
	}
	if ((size_t)frames_offset + frame_count * 16 > f.size()) return false;
	e.frames.resize(frame_count);
	for (size_t i = 0; i != frame_count; ++i) {
		const uint8_t* p = &f[frames_offset + i * 16];
		e.frames[i] = {le16(p), le16(p + 2), (int16_t)le16(p + 4), (int16_t)le16(p + 6), le16(p + 8), le16(p + 10)};
	}
	return true;
}

inline bool anim_header(const bytes& f, int& scale, size_t& entries, std::vector<std::string>& layer_names) {
	if (f.size() < 0x14c || std::memcmp(f.data(), "ANIM", 4) != 0) return false;
	scale = f[4];
	size_t layers = le16(&f[8]);
	entries = le16(&f[10]);
	if (layers > 10) return false;
	layer_names.clear();
	for (size_t i = 0; i != layers; ++i) {
		const char* n = (const char*)&f[12 + i * 32];
		layer_names.emplace_back(n, strnlen(n, 32));
	}
	return true;
}

}

// An HD/HD2 per-image anim file (one entry).
inline bool parse_anim(const bytes& f, anim_entry& out) {
	int scale;
	size_t entries;
	std::vector<std::string> names;
	if (!detail::anim_header(f, scale, entries, names) || entries != 1) return false;
	return detail::parse_anim_entry(f, 0x14c, scale, names, out);
}

// SD/mainSD.anim: image id -> referenced image id (or -1). Graphics are not decoded.
inline std::vector<int> parse_sd_anim_references(const bytes& f) {
	std::vector<int> refs;
	int scale;
	size_t entries;
	std::vector<std::string> names;
	if (!detail::anim_header(f, scale, entries, names)) return refs;
	if (0x14c + entries * 4 > f.size()) return refs;
	refs.assign(entries, -1);
	for (size_t i = 0; i != entries; ++i) {
		size_t pos = detail::le32(&f[0x14c + i * 4]);
		if (pos + 4 <= f.size() && detail::le16(&f[pos]) == 0) refs[i] = detail::le16(&f[pos + 2]);
	}
	return refs;
}

// ---- .dds.grp / .dds.vr4 ----

struct dds_grp {
	struct frame {
		int w, h;
		size_t offset, size;  // the frame's DDS file within `data`
	};
	bytes data;
	std::vector<frame> frames;

	bool decode(size_t index, image& out) const {
		if (index >= frames.size()) return false;
		auto& f = frames[index];
		return decode_dds(data.data() + f.offset, f.size, out);
	}
};

// Header: u32 file size, u16 frame count, u8 scale, u8 version; then per frame
// u32 unknown, u16 width, u16 height, u32 size, followed by that many bytes of DDS.
// `out` is replaced only on success; a truncated file leaves it empty.
inline bool parse_dds_grp(bytes data, dds_grp& out) {
	using namespace detail;
	out = {};
	if (data.size() < 8) return false;
	size_t count = le16(&data[4]);
	size_t pos = 8;
	std::vector<dds_grp::frame> frames;
	for (size_t i = 0; i != count; ++i) {
		if (pos + 12 > data.size()) return false;
		dds_grp::frame f{le16(&data[pos + 4]), le16(&data[pos + 6]), pos + 12, le32(&data[pos + 8])};
		if (f.offset + f.size > data.size()) return false;
		frames.push_back(f);
		pos = f.offset + f.size;
	}
	out.frames = std::move(frames);
	out.data = std::move(data);
	return true;
}

// ---- resampling ----

// Separable resampler: each output pixel averages the source under a tent filter whose
// radius is the larger of one source texel and one output pixel (area-like when
// shrinking, bilinear when enlarging). `get(x, y)` returns premultiplied RGBA (or any
// four 8-bit channels); the result has the same layout.
struct resample_axis {
	struct tap {
		int index;
		float weight;
	};
	std::vector<int> start;  // per output: first tap in `taps`
	std::vector<tap> taps;

	// Output pixel i covers source coordinates [src0 + i / k, src0 + (i + 1) / k).
	// `clamp_edges`: renormalise at the source edges (opaque tiles that abut seamlessly);
	// otherwise the outside counts as transparent (sprites, whose edges fade out).
	resample_axis(int src_len, int dst_len, double k, bool clamp_edges, double src0 = 0.0) {
		double inv = 1.0 / k;
		double radius = std::max(1.0, inv);
		start.reserve(dst_len + 1);
		for (int i = 0; i != dst_len; ++i) {
			start.push_back((int)taps.size());
			double centre = src0 + (i + 0.5) * inv - 0.5;
			int lo = (int)std::floor(centre - radius) + 1, hi = (int)std::ceil(centre + radius) - 1;
			// Normalise by the weight of the whole support (taps outside the source count
			// as transparent) or, with clamp_edges, of the taps inside it. Dividing by the
			// radius instead is only exact for integer radii and ripples otherwise.
			double inside = 0, support = 0;
			size_t first = taps.size();
			for (int s = lo; s <= hi; ++s) {
				double w = 1.0 - std::abs(s - centre) / radius;
				if (w <= 0) continue;
				support += w;
				if (s < 0 || s >= src_len) continue;
				taps.push_back({s, (float)w});
				inside += w;
			}
			double norm = clamp_edges ? inside : support;
			if (norm <= 0) norm = 1;
			for (size_t t = first; t != taps.size(); ++t) taps[t].weight = (float)(taps[t].weight / norm);
		}
		start.push_back((int)taps.size());
	}
};

// Resamples the `sw` x `sh` region at (sx, sy) of `src` to `dw` x `dh`.
inline image resample(const image& src, int sx, int sy, int sw, int sh, int dw, int dh, bool clamp_edges) {
	image out;
	out.w = dw;
	out.h = dh;
	out.px.assign((size_t)dw * dh, 0);
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return out;
	resample_axis ax(sw, dw, (double)dw / sw, clamp_edges), ay(sh, dh, (double)dh / sh, clamp_edges);
	std::vector<float> row((size_t)dw * 4);
	std::vector<float> tmp((size_t)dw * sh * 4);
	for (int y = 0; y != sh; ++y) {
		const uint32_t* s = &src.px[(size_t)(sy + y) * src.w + sx];
		float* t = &tmp[(size_t)y * dw * 4];
		for (int x = 0; x != dw; ++x) {
			float c[4] = {0, 0, 0, 0};
			for (int k = ax.start[x]; k != ax.start[x + 1]; ++k) {
				uint32_t p = s[ax.taps[k].index];
				float w = ax.taps[k].weight;
				c[0] += ch_r(p) * w;
				c[1] += ch_g(p) * w;
				c[2] += ch_b(p) * w;
				c[3] += ch_a(p) * w;
			}
			std::memcpy(t + x * 4, c, sizeof(c));
		}
	}
	for (int y = 0; y != dh; ++y) {
		std::fill(row.begin(), row.end(), 0.0f);
		for (int k = ay.start[y]; k != ay.start[y + 1]; ++k) {
			const float* t = &tmp[(size_t)ay.taps[k].index * dw * 4];
			float w = ay.taps[k].weight;
			for (int i = 0; i != dw * 4; ++i) row[i] += t[i] * w;
		}
		uint32_t* o = &out.px[(size_t)y * dw];
		for (int x = 0; x != dw; ++x) {
			auto q = [&](int c) { return (uint32_t)std::min(255.0f, std::max(0.0f, row[x * 4 + c] + 0.5f)); };
			o[x] = rgba(q(0), q(1), q(2), q(3));
		}
	}
	return out;
}

}

#endif
