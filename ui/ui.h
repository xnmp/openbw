#include "common.h"
#include "../bwgame.h"
#include "../replay.h"

#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"
#include "hud_font.h"
#include "order_names.h"
#ifdef OPENBW_REMASTERED
#include "scr_render.h"
#endif

#include <climits>
#include <functional>
#ifdef _WIN32
#include <direct.h>
#elif !defined(EMSCRIPTEN)
#include <sys/stat.h>
#endif

namespace bwgame {

struct vr4_entry {
	using bitmap_t = std::conditional<is_native_fast_int<uint64_t>::value, uint64_t, uint32_t>::type;
	std::array<bitmap_t, 64 / sizeof(bitmap_t)> bitmap;
	std::array<bitmap_t, 64 / sizeof(bitmap_t)> inverted_bitmap;
};
struct vx4_entry {
	std::array<uint16_t, 16> images;
};

struct pcx_image {
	size_t width;
	size_t height;
	a_vector<uint8_t> data;
};

struct tileset_image_data {
	a_vector<uint8_t> wpe;
	a_vector<vr4_entry> vr4;
	a_vector<vx4_entry> vx4;
	pcx_image dark_pcx;
	std::array<pcx_image, 7> light_pcx;
	grp_t creep_grp;
	int resource_minimap_color;
	std::array<uint8_t, 256> cloak_fade_selector;
};

struct image_data {
	std::array<std::array<uint8_t, 8>, 16> player_unit_colors;
	std::array<uint8_t, 16> player_minimap_colors;
	std::array<uint8_t, 24> selection_colors;
	std::array<uint8_t, 24> hp_bar_colors;
	std::array<int, 0x100> creep_edge_frame_index{};
};

template<typename data_T>
pcx_image load_pcx_data(const data_T& data) {
	data_loading::data_reader_le r(data.data(), data.data() + data.size());
	auto base_r = r;
	auto id = r.get<uint8_t>();
	if (id != 0x0a) error("pcx: invalid identifier %#x", id);
	r.get<uint8_t>(); // version
	auto encoding = r.get<uint8_t>(); // encoding
	auto bpp = r.get<uint8_t>(); // bpp
	auto offset_x = r.get<uint16_t>();
	auto offset_y = r.get<uint16_t>();
	auto last_x = r.get<uint16_t>();
	auto last_y = r.get<uint16_t>();

	if (encoding != 1) error("pcx: invalid encoding %#x", encoding);
	if (bpp != 8) error("pcx: bpp is %d, expected 8", bpp);

	if (offset_x != 0 || offset_y != 0) error("pcx: offset %d %d, expected 0 0", offset_x, offset_y);

	r.skip(2 + 2 + 48 + 1);

	auto bit_planes = r.get<uint8_t>();
	auto bytes_per_line = r.get<uint16_t>();

	size_t width = last_x + 1;
	size_t height = last_y + 1;

	pcx_image pcx;
	pcx.width = width;
	pcx.height = height;

	pcx.data.resize(width * height);

	r = base_r;
	r.skip(128);

	auto padding = bytes_per_line * bit_planes - width;
	if (padding != 0) error("pcx: padding not supported");

	uint8_t* dst = pcx.data.data();
	uint8_t* dst_end = pcx.data.data() + pcx.data.size();

	while (dst != dst_end) {
		auto v = r.get<uint8_t>();
		if ((v & 0xc0) == 0xc0) {
			v &= 0x3f;
			auto c = r.get<uint8_t>();
			for (; v; --v) {
				if (dst == dst_end) error("pcx: failed to decode");
				*dst++ = c;
			}
		} else {
			*dst = v;
			++dst;
		}
	}

	return pcx;
}

static inline std::unique_ptr<native_window_drawing::surface> flip_image(native_window_drawing::surface* src) {
	auto tmp = native_window_drawing::create_rgba_surface(src->w, src->h);
	src->blit(&*tmp, 0, 0);
	void* ptr = tmp->lock();
	uint32_t* pixels = (uint32_t*)ptr;
	for (size_t y = 0; y != (size_t)tmp->h; ++y) {
		for (size_t x = 0; x != (size_t)tmp->w / 2; ++x) {
			std::swap(pixels[x], pixels[tmp->w - 1 - x]);
		}
		pixels += tmp->pitch / 4;
	}
	tmp->unlock();
	return tmp;
}

template<typename load_data_file_F>
void load_image_data(image_data& img, load_data_file_F&& load_data_file) {

	std::array<int, 0x100> creep_edge_neighbors_index{};
	std::array<int, 128> creep_edge_neighbors_index_n{};

	for (size_t i = 0; i != 0x100; ++i) {
		int v = 0;
		if (i & 2) v |= 0x10;
		if (i & 8) v |= 0x24;
		if (i & 0x10) v |= 9;
		if (i & 0x40) v |= 2;
		if ((i & 0xc0) == 0xc0) v |= 1;
		if ((i & 0x60) == 0x60) v |= 4;
		if ((i & 3) == 3) v |= 0x20;
		if ((i & 6) == 6) v |= 8;
		if ((v & 0x21) == 0x21) v |= 0x40;
		if ((v & 0xc) == 0xc) v |= 0x40;
		creep_edge_neighbors_index[i] = v;
	}

	int n = 0;
	for (int i = 0; i != 128; ++i) {
		auto it = std::find(creep_edge_neighbors_index.begin(), creep_edge_neighbors_index.end(), i);
		if (it == creep_edge_neighbors_index.end()) continue;
		creep_edge_neighbors_index_n[i] = n;
		++n;
	}

	for (size_t i = 0; i != 0x100; ++i) {
		img.creep_edge_frame_index[i] = creep_edge_neighbors_index_n[creep_edge_neighbors_index[i]];
	}

	a_vector<uint8_t> tmp_data;
	auto load_pcx_file = [&](a_string filename) {
		load_data_file(tmp_data, std::move(filename));
		return load_pcx_data(tmp_data);
	};

	auto tunit_pcx = load_pcx_file("game/tunit.pcx");
	if (tunit_pcx.width != 128 || tunit_pcx.height != 1) error("tunit.pcx dimensions are %dx%d (128x1 required)", tunit_pcx.width, tunit_pcx.height);
	for (size_t i = 0; i != 16; ++i) {
		for (size_t i2 = 0; i2 != 8; ++i2) {
			img.player_unit_colors[i][i2] = tunit_pcx.data[i * 8 + i2];
		}
	}
	auto tminimap_pcx = load_pcx_file("game/tminimap.pcx");
	if (tminimap_pcx.width != 16 || tminimap_pcx.height != 1) error("tminimap.pcx dimensions are %dx%d (16x1 required)", tminimap_pcx.width, tminimap_pcx.height);
	for (size_t i = 0; i != 16; ++i) {
		img.player_minimap_colors[i] = tminimap_pcx.data[i];
	}
	auto tselect_pcx = load_pcx_file("game/tselect.pcx");
	if (tselect_pcx.width != 24 || tselect_pcx.height != 1) error("tselect.pcx dimensions are %dx%d (24x1 required)", tselect_pcx.width, tselect_pcx.height);
	for (size_t i = 0; i != 24; ++i) {
		img.selection_colors[i] = tselect_pcx.data[i];
	}
	auto thpbar_pcx = load_pcx_file("game/thpbar.pcx");
	if (thpbar_pcx.width != 19 || thpbar_pcx.height != 1) error("thpbar.pcx dimensions are %dx%d (19x1 required)", thpbar_pcx.width, thpbar_pcx.height);
	for (size_t i = 0; i != 19; ++i) {
		img.hp_bar_colors[i] = thpbar_pcx.data[i];
	}

}

template<typename load_data_file_F>
void load_tileset_image_data(tileset_image_data& img, size_t tileset_index, load_data_file_F&& load_data_file) {
	using namespace data_loading;

	std::array<const char*, 8> tileset_names = {
		"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"
	};

	a_vector<uint8_t> vr4_data;
	a_vector<uint8_t> vx4_data;

	const char* tileset_name = tileset_names.at(tileset_index);

	load_data_file(vr4_data, format("Tileset/%s.vr4", tileset_name));
	load_data_file(vx4_data, format("Tileset/%s.vx4", tileset_name));
	load_data_file(img.wpe, format("Tileset/%s.wpe", tileset_name));

	a_vector<uint8_t> grp_data;
	load_data_file(grp_data, format("Tileset/%s.grp", tileset_name));
	img.creep_grp = read_grp(data_loading::data_reader_le(grp_data.data(), grp_data.data() + grp_data.size()));

	data_reader<true, false> vr4_r(vr4_data.data(), nullptr);
	img.vr4.resize(vr4_data.size() / 64);
	for (size_t i = 0; i != img.vr4.size(); ++i) {
		for (size_t i2 = 0; i2 != 8; ++i2) {
			auto r2 = vr4_r;
			auto v = vr4_r.get<uint64_t, true>();
			auto iv = r2.get<uint64_t, false>();
			size_t n = 8 / sizeof(vr4_entry::bitmap_t);
			for (size_t i3 = 0; i3 != n; ++i3) {
				img.vr4[i].bitmap[i2 * n + i3] = (vr4_entry::bitmap_t)v;
				img.vr4[i].inverted_bitmap[i2 * n + i3] = (vr4_entry::bitmap_t)iv;
				v >>= n == 1 ? 0 : 8 * sizeof(vr4_entry::bitmap_t);
				iv >>= n == 1 ? 0 : 8 * sizeof(vr4_entry::bitmap_t);
			}
		}
	}
	data_reader<true, false> vx4_r(vx4_data.data(), vx4_data.data() + vx4_data.size());
	img.vx4.resize(vx4_data.size() / 32);
	for (size_t i = 0; i != img.vx4.size(); ++i) {
		for (size_t i2 = 0; i2 != 16; ++i2) {
			img.vx4[i].images[i2] = vx4_r.get<uint16_t>();
		}
	}

	a_vector<uint8_t> tmp_data;
	auto load_pcx_file = [&](a_string filename) {
		load_data_file(tmp_data, std::move(filename));
		return load_pcx_data(tmp_data);
	};

	img.dark_pcx = load_pcx_file(format("Tileset/%s/dark.pcx", tileset_name));
	if (img.dark_pcx.width != 256 || img.dark_pcx.height != 32) error("invalid dark.pcx");
	for (size_t x = 0; x != 256; ++x) {
		img.dark_pcx.data[256 * 31 + x] = (uint8_t)x;
	}

	std::array<const char*, 7> light_names = {"ofire", "gfire", "bfire", "bexpl", "trans50", "red", "green"};
	for (size_t i = 0; i != 7; ++i) {
		img.light_pcx[i] = load_pcx_file(format("Tileset/%s/%s.pcx", tileset_name, light_names[i]));
	}

	if (img.wpe.size() != 256 * 4) error("wpe size invalid (%d)", img.wpe.size());

	auto get_nearest_color = [&](int r, int g, int b) {
		size_t best_index = -1;
		int best_score{};
		for (size_t i = 0; i != 256; ++i) {
			int dr = r - img.wpe[4 * i + 0];
			int dg = g - img.wpe[4 * i + 1];
			int db = b - img.wpe[4 * i + 2];
			int score = dr * dr + dg * dg + db * db;
			if (best_index == (size_t)-1 || score < best_score) {
				best_index = i;
				best_score = score;
			}
		}
		return best_index;
	};
	img.resource_minimap_color = get_nearest_color(0, 255, 255);

	for (size_t i = 0; i != 256; ++i) {
		int r = img.wpe[4 * i + 0];
		int g = img.wpe[4 * i + 1];
		int b = img.wpe[4 * i + 2];
		int strength = (r * 28 + g * 77 + b * 151 + 4096) / 8192;
		img.cloak_fade_selector[i] = strength;
	}

}

template<bool bounds_check>
void draw_tile(tileset_image_data& img, size_t megatile_index, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height) {
	auto* images = &img.vx4.at(megatile_index).images[0];
	size_t x = 0;
	size_t y = 0;
	for (size_t image_iy = 0; image_iy != 4; ++image_iy) {
		for (size_t image_ix = 0; image_ix != 4; ++image_ix) {
			auto image_index = *images;
			bool inverted = (image_index & 1) == 1;
			auto* bitmap = inverted ? &img.vr4.at(image_index / 2).inverted_bitmap[0] : &img.vr4.at(image_index / 2).bitmap[0];

			for (size_t iy = 0; iy != 8; ++iy) {
				for (size_t iv = 0; iv != 8 / sizeof(vr4_entry::bitmap_t); ++iv) {
					for (size_t b = 0; b != sizeof(vr4_entry::bitmap_t); ++b) {
						if (!bounds_check || (x >= offset_x && y >= offset_y && x < width && y < height)) {
							*dst = (uint8_t)(*bitmap >> (8 * b));
						}
						++dst;
						++x;
					}
					++bitmap;
				}
				x -= 8;
				++y;
				dst -= 8;
				dst += pitch;
			}
			x += 8;
			y -= 8;
			dst -= pitch * 8;
			dst += 8;
			++images;
		}
		x -= 32;
		y += 8;
		dst += pitch * 8;
		dst -= 32;
	}
}

static inline void draw_tile(tileset_image_data& img, size_t megatile_index, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height) {
	if (offset_x == 0 && offset_y == 0 && width == 32 && height == 32) {
		draw_tile<false>(img, megatile_index, dst, pitch, offset_x, offset_y, width, height);
	} else {
		draw_tile<true>(img, megatile_index, dst, pitch, offset_x, offset_y, width, height);
	}
}

template<bool bounds_check, bool flipped, bool textured, typename remap_F>
void draw_frame(const grp_t::frame_t& frame, const uint8_t* texture, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f) {
	for (size_t y = 0; y != offset_y; ++y) {
		dst += pitch;
		if (textured) texture += frame.size.x;
	}

	for (size_t y = offset_y; y != height; ++y) {

		if (flipped) dst += frame.size.x - 1;
		if (textured && flipped) texture += frame.size.x - 1;

		const uint8_t* d = frame.data_container.data() + frame.line_data_offset.at(y);
		for (size_t x = flipped ? frame.size.x - 1 : 0; x != (flipped ? (size_t)0 - 1 : frame.size.x);) {
			int v = *d++;
			if (v & 0x80) {
				v &= 0x7f;
				x += flipped ? -v : v;
				dst += flipped ? -v : v;
				if (textured) texture += flipped ? -v : v;
			} else if (v & 0x40) {
				v &= 0x3f;
				int c = *d++;
				for (;v; --v) {
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f(textured ? *texture : c, *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			} else {
				for (;v; --v) {
					int c = *d++;
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f(textured ? *texture : c, *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			}
		}

		if (!flipped) dst -= frame.size.x;
		else ++dst;
		dst += pitch;
		if (textured) {
			if (!flipped) texture -= frame.size.x;
			else ++texture;
			texture += frame.size.x;
		}

	}
}

struct no_remap {
	uint8_t operator()(uint8_t new_value, uint8_t old_value) const {
		return new_value;
	}
};

template<typename remap_F = no_remap>
void draw_frame(const grp_t::frame_t& frame, bool flipped, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f = remap_F()) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame<false, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<false, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame<true, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<true, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

template<typename remap_F = no_remap>
void draw_frame_textured(const grp_t::frame_t& frame, const uint8_t* texture, bool flipped, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f = remap_F()) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame<false, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<false, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame<true, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<true, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

struct apm_t {
	a_deque<int> history;
	int current_apm = 0;
	int last_frame_div = 0;
	static const int resolution = 1;
	void add_action(int frame) {
		if (!history.empty() && frame / resolution == last_frame_div) {
			++history.back();
		} else {
			if (history.size() >= 10 * 1000 / 42 / resolution) history.pop_front();
			history.push_back(1);
			last_frame_div = frame / 12;
		}
	}
	void update(int frame) {
		if (history.empty() || frame / resolution != last_frame_div) {
			if (history.size() >= 10 * 1000 / 42 / resolution) history.pop_front();
			history.push_back(0);
			last_frame_div = frame / resolution;
		}
		if (frame % resolution) return;
		if (history.size() == 0) {
			current_apm = 0;
			return;
		}
		int sum = 0;
		for (auto& v : history) sum += v;
		current_apm = (int)(sum * ((int64_t)256 * 60 * 1000 / 42 / resolution) / history.size() / 256);
	}
};

struct ui_util_functions: replay_functions {

	explicit ui_util_functions(state& st, action_state& action_st, replay_state& replay_st) : replay_functions(st, action_st, replay_st) {}

	rect sprite_clickable_bounds(const sprite_t* sprite) const {
		rect r{{(int)game_st.map_width - 1, (int)game_st.map_height - 1}, {0, 0}};
		for (const image_t* image : ptr(sprite->images)) {
			if (!i_flag(image, image_t::flag_clickable)) continue;
			xy pos = get_image_map_position(image);
			auto size = image->grp->frames.at(image->frame_index).size;
			xy to = pos + xy((int)size.x, (int)size.y);
			if (pos.x < r.from.x) r.from.x = pos.x;
			if (pos.y < r.from.y) r.from.y = pos.y;
			if (to.x > r.to.x) r.to.x = to.x;
			if (to.y > r.to.y) r.to.y = to.y;
		}
		return r;
	}

	bool unit_can_be_selected(const unit_t* u) const {
		if (unit_is(u, UnitTypes::Terran_Nuclear_Missile)) return false;
		if (unit_is(u, UnitTypes::Protoss_Scarab)) return false;
		if (unit_is(u, UnitTypes::Spell_Disruption_Web)) return false;
		if (unit_is(u, UnitTypes::Spell_Dark_Swarm)) return false;
		if (unit_is(u, UnitTypes::Special_Upper_Level_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Right_Upper_Level_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Pit_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Right_Pit_Door)) return false;
		return true;
	}

	bool image_has_data_at(const image_t* image, xy pos) const {
		auto& frame = image->grp->frames.at(image->frame_index);
		xy map_pos = get_image_map_position(image);
		int x = pos.x - map_pos.x;
		if (i_flag(image, image_t::flag_horizontally_flipped)) x = image->grp->width - 2 * frame.offset.x - x;
		int y = pos.y - map_pos.y;
		if ((size_t)x >= frame.size.x) return false;
		if ((size_t)y >= frame.size.y) return false;

		const uint8_t* d = frame.data_container.data() + frame.line_data_offset.at(y);
		while (x > 0) {
			int v = *d++;
			if (v & 0x80) {
				v &= 0x7f;
				x -= v;
				if (x <= 0) return false;
			} else if (v & 0x40) {
				v &= 0x3f;
				d++;
				x -= v;
			} else {
				x -= v;
			}
		}
		return true;
	}

	bool unit_has_clickable_image_data_at(const unit_t* u, xy pos) const {
		if (!is_in_bounds(pos, sprite_clickable_bounds(u->sprite))) return false;
		if (ut_flag(u, unit_type_t::flag_100)) {
			for (const image_t* image : ptr(u->sprite->images)) {
				if (!i_flag(image, image_t::flag_clickable)) continue;
				if (image_has_data_at(image, pos)) return true;
			}
			return false;
		} else {
			return image_has_data_at(u->sprite->main_image, pos);
		}
	}

	unit_t* select_get_unit_at(xy pos) const {
		rect area = square_at(pos, 32);
		area.to += xy(game_st.max_unit_width, game_st.max_unit_height);
		unit_t* best_unit = nullptr;
		int best_unit_size{};
		for (unit_t* u : find_units_noexpand(area)) {
			if (!is_in_bounds(pos, sprite_clickable_bounds(u->sprite))) continue;
			u = unit_main_unit(u);
			if (!unit_can_be_selected(u)) continue;
			if (!best_unit) {
				best_unit = u;
				best_unit_size = u->unit_type->placement_size.x * u->unit_type->placement_size.y;
				continue;
			}
			if (sprite_depth_order(u->sprite) >= sprite_depth_order(best_unit->sprite)) {
				if (unit_has_clickable_image_data_at(u, pos) || (u->subunit && unit_has_clickable_image_data_at(u->subunit, pos))) {
					best_unit = u;
					best_unit_size = u->sprite->width * u->sprite->height;
					continue;
				}
			} else {
				if (unit_has_clickable_image_data_at(best_unit, pos)) continue;
				if (best_unit->subunit && unit_has_clickable_image_data_at(best_unit->subunit, pos)) continue;
			}
			if (u->unit_type->placement_size.x * u->unit_type->placement_size.y < best_unit_size) {
				best_unit = u;
				best_unit_size = u->unit_type->placement_size.x * u->unit_type->placement_size.y;
			}
		}
		return best_unit;
	}

	uint32_t sprite_depth_order(const sprite_t* sprite) const {
		uint32_t score = 0;
		score |= sprite->elevation_level;
		score <<= 13;
		score |= sprite->elevation_level <= 4 ? sprite->position.y : 0;
		score <<= 1;
		score |= s_flag(sprite, sprite_t::flag_turret) ? 1 : 0;
		return score;
	}

};

struct ui_functions: ui_util_functions {
	image_data img;
	tileset_image_data tileset_img;
	native_window::window wnd;
	bool create_window = true;
	bool draw_ui_elements = true;

	bool exit_on_close = true;
	bool window_closed = false;

	xy screen_pos;

	size_t screen_width;
	size_t screen_height;

	game_player player;
	replay_state current_replay_state;
	action_state current_action_state;
	std::array<apm_t, 12> apm;
	ui_functions(game_player player) : ui_util_functions(player.st(), current_action_state, current_replay_state), player(std::move(player)) {
	}

	std::function<void(a_vector<uint8_t>&, a_string)> load_data_file;

	sound_types_t sound_types;
	a_vector<a_string> sound_filenames;

	const sound_type_t* get_sound_type(Sounds id) const {
		if ((size_t)id >= (size_t)Sounds::None) error("invalid sound id %d", (size_t)id);
		return &sound_types.vec[(size_t)id];
	}

	a_vector<bool> has_loaded_sound;
	a_vector<std::unique_ptr<native_sound::sound>> loaded_sounds;
	a_vector<std::chrono::high_resolution_clock::time_point> last_played_sound;

	int global_volume = 50;

	struct sound_channel {
		bool playing = false;
		const sound_type_t* sound_type = nullptr;
		int priority = 0;
		int flags = 0;
		const unit_type_t* unit_type = nullptr;
		int volume = 0;
	};
	a_vector<sound_channel> sound_channels;

	void set_volume(int volume) {
		if (volume < 0) volume = 0;
		else if (volume > 100) volume = 100;
		global_volume = volume;
		for (auto& c : sound_channels) {
			if (c.playing) {
				native_sound::set_volume(&c - sound_channels.data(), (128 - 4) * (c.volume * global_volume / 100) / 100);
			}
		}
	}

	sound_channel* get_sound_channel(int priority) {
		sound_channel* r = nullptr;
		for (auto& c : sound_channels) {
			if (c.playing) {
				if (!native_sound::is_playing(&c - sound_channels.data())) {
					c.playing = false;
					r = &c;
				}
			} else r = &c;
		}
		if (r) return r;
		int best_prio = priority;
		for (auto& c : sound_channels) {
			if (c.flags & 0x20) continue;
			if (c.priority < best_prio) {
				best_prio = c.priority;
				r = &c;
			}
		}
		return r;
	}

	virtual void play_sound(int id, xy position, const unit_t* source_unit, bool add_race_index) override {
		if (global_volume == 0) return;
		if (add_race_index) id += 1;
		if ((size_t)id >= has_loaded_sound.size()) return;
		if (!has_loaded_sound[id]) {
			has_loaded_sound[id] = true;
			a_vector<uint8_t> data;
			load_data_file(data, "sound/" + sound_filenames[id]);
			loaded_sounds[id] = native_sound::load_wav(data.data(), data.size());
		}
		auto& s = loaded_sounds[id];
		if (!s) return;

		auto now = clock.now();
		if (now - last_played_sound[id] <= std::chrono::milliseconds(80)) return;
		last_played_sound[id] = now;

		const sound_type_t* sound_type = get_sound_type((Sounds)id);

		int volume = sound_type->min_volume;

		if (position != xy()) {
			int distance = 0;
			if (position.x < screen_pos.x) distance += screen_pos.x - position.x;
			else if (position.x > screen_pos.x + (int)screen_width) distance += position.x - (screen_pos.x + (int)screen_width);
			if (position.y < screen_pos.y) distance += screen_pos.y - position.y;
			else if (position.y > screen_pos.y + (int)screen_height) distance += position.y - (screen_pos.y + (int)screen_height);

			int distance_volume = 99 - 99 * distance / 512;

			if (distance_volume > volume) volume = distance_volume;
		}

		if (volume > 10) {
			int pan = 0;
//			if (position != xy()) {
//				int pan_x = (position.x - (screen_pos.x + (int)screen_width / 2)) / 32;
//				bool left = pan_x < 0;
//				if (left) pan_x = -pan_x;
//				if (pan_x <= 2) pan = 0;
//				else if (pan_x <= 5) pan = 52;
//				else if (pan_x <= 10) pan = 127;
//				else if (pan_x <= 20) pan = 191;
//				else if (pan_x <= 40) pan = 230;
//				else pan = 255;
//				if (left) pan = -pan;
//			}

			const unit_type_t* unit_type = source_unit ? source_unit->unit_type : nullptr;

			if (sound_type->flags & 0x10) {
				for (auto& c : sound_channels) {
					if (c.playing && c.sound_type == sound_type) {
						if (native_sound::is_playing(&c - sound_channels.data())) return;
						c.playing = false;
					}
				}
			} else if (sound_type->flags & 2 && unit_type) {
				for (auto& c : sound_channels) {
					if (c.playing && c.unit_type == unit_type && c.flags & 2) {
						if (native_sound::is_playing(&c - sound_channels.data())) return;
						c.playing = false;
					}
				}
			}

			auto* c = get_sound_channel(sound_type->priority);
			if (c) {
				native_sound::play(c - sound_channels.data(), &*s, (128 - 4) * (volume * global_volume / 100) / 100, pan);
				c->playing = true;
				c->sound_type = sound_type;
				c->flags = sound_type->flags;
				c->priority = sound_type->priority;
				c->unit_type = unit_type;
				c->volume = volume;
			}
		}
	}

	a_vector<uint8_t> creep_random_tile_indices = a_vector<uint8_t>(256 * 256);
	void init() {
		load_selection_panel_data();
		uint32_t rand_state = (uint32_t)clock.now().time_since_epoch().count();
		auto rand = [&]() {
			rand_state = rand_state * 22695477 + 1;
			return (rand_state >> 16) & 0x7fff;
		};
		for (auto& v : creep_random_tile_indices) {
			if (rand() % 100 < 4) v = 6 + rand() % 7;
			else v = rand() % 6;
		}

		a_vector<uint8_t> data;
		load_data_file(data, "arr/sfxdata.dat");
		sound_types = data_loading::load_sfxdata_dat(data);

		sound_filenames.resize(sound_types.vec.size());
		has_loaded_sound.resize(sound_filenames.size());
		loaded_sounds.resize(has_loaded_sound.size());
		last_played_sound.resize(loaded_sounds.size());

		string_table_data tbl;
		load_data_file(tbl.data, "arr/sfxdata.tbl");
		for (size_t i = 0; i != sound_types.vec.size(); ++i) {
			size_t index = sound_types.vec[i].filename_index;
			sound_filenames[i] = tbl[index];
		}
		native_sound::frequency = 44100;
		native_sound::channels = 8;
		native_sound::init();

		sound_channels.resize(8);

		load_data_file(images_tbl.data, "arr/images.tbl");

		load_all_image_data(load_data_file);
	}

	virtual void on_action(int owner, int action) override {
		apm.at(owner).add_action(st.current_frame);
	}

	size_t view_width;
	size_t view_height;
	fp16 view_scale;

	rect_t<xy_t<size_t>> screen_tile_bounds() {
		size_t from_tile_y = screen_pos.y / 32u;
		if (from_tile_y >= game_st.map_tile_height) from_tile_y = 0;
		size_t to_tile_y = (screen_pos.y + view_height + 31) / 32u;
		if (to_tile_y > game_st.map_tile_height) to_tile_y = game_st.map_tile_height;
		size_t from_tile_x = screen_pos.x / 32u;
		if (from_tile_x >= game_st.map_tile_width) from_tile_x = 0;
		size_t to_tile_x = (screen_pos.x + view_width + 31) / 32u;
		if (to_tile_x > game_st.map_tile_width) to_tile_x = game_st.map_tile_width;

		return {{from_tile_x, from_tile_y}, {to_tile_x, to_tile_y}};
	}


	void draw_tiles(uint8_t* data, size_t data_pitch) {

		auto screen_tile = screen_tile_bounds();

		size_t tile_index = screen_tile.from.y * game_st.map_tile_width + screen_tile.from.x;
		auto* megatile_index = &st.tiles_mega_tile_index[tile_index];
		auto* tile = &st.tiles[tile_index];
		size_t width = screen_tile.to.x - screen_tile.from.x;

		xy dirs[9] = {{1, 1}, {0, 1}, {-1, 1}, {1, 0}, {-1, 0}, {1, -1}, {0, -1}, {-1, -1}, {0, 0}};

		for (size_t tile_y = screen_tile.from.y; tile_y != screen_tile.to.y; ++tile_y) {
			for (size_t tile_x = screen_tile.from.x; tile_x != screen_tile.to.x; ++tile_x) {

				int screen_x = tile_x * 32 - screen_pos.x;
				int screen_y = tile_y * 32 - screen_pos.y;

				size_t offset_x = 0;
				size_t offset_y = 0;
				if (screen_x < 0) {
					offset_x = -screen_x;
				}
				if (screen_y < 0) {
					offset_y = -screen_y;
				}

				uint8_t* dst = data + screen_y * data_pitch + screen_x;

				size_t width = 32;
				size_t height = 32;

				width = std::min(width, screen_width - screen_x);
				height = std::min(height, screen_height - screen_y);

				size_t index = *megatile_index;
				if (tile->flags & tile_t::flag_has_creep) {
					index = game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[tile_x + tile_y * game_st.map_tile_width]];
				}
				draw_tile(tileset_img, index, dst, data_pitch, offset_x, offset_y, width, height);

				if (~tile->flags & tile_t::flag_has_creep) {
					size_t creep_index = 0;
					for (size_t i = 0; i != 9; ++i) {
						int add_x = dirs[i].x;
						int add_y = dirs[i].y;
						if (tile_x + add_x >= game_st.map_tile_width) continue;
						if (tile_y + add_y >= game_st.map_tile_height) continue;
						if (st.tiles[tile_x + add_x + (tile_y + add_y) * game_st.map_tile_width].flags & tile_t::flag_has_creep) creep_index |= 1 << i;
					}
					size_t creep_frame = img.creep_edge_frame_index[creep_index];

					if (creep_frame) {

						auto& frame = tileset_img.creep_grp.frames.at(creep_frame - 1);

						screen_x += frame.offset.x;
						screen_y += frame.offset.y;

						size_t width = frame.size.x;
						size_t height = frame.size.y;

						if (screen_x < (int)screen_width && screen_y < (int)screen_height) {
							if (screen_x + (int)width > 0 && screen_y + (int)height > 0) {

								size_t offset_x = 0;
								size_t offset_y = 0;
								if (screen_x < 0) {
									offset_x = -screen_x;
								}
								if (screen_y < 0) {
									offset_y = -screen_y;
								}

								uint8_t* dst = data + screen_y * data_pitch + screen_x;

								width = std::min(width, screen_width - screen_x);
								height = std::min(height, screen_height - screen_y);

								draw_frame(frame, false, dst, data_pitch, offset_x, offset_y, width, height);
							}
						}
					}
				}

				++megatile_index;
				++tile;
			}
			megatile_index -= width;
			megatile_index += game_st.map_tile_width;
			tile -= width;
			tile += game_st.map_tile_width;
		}
	}

	a_vector<uint8_t> temporary_warp_texture_buffer;

	void draw_image(const image_t* image, uint8_t* data, size_t data_pitch, size_t color_index) {

		if (is_new_image(image)) {
			image_draw_queue.push_back(image);
			return;
		}

		xy map_pos = get_image_map_position(image);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;

		auto& frame = image->grp->frames.at(image->frame_index);

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		auto draw_alpha = [&](size_t index, auto remap_f) {
			auto& data = tileset_img.light_pcx.at(index).data;
			uint8_t* ptr = data.data();
			size_t size = data.size() / 256;
			auto glow = [ptr, size, remap_f](uint8_t new_value, uint8_t old_value) {
				new_value = remap_f(new_value, old_value);
				--new_value;
				if (new_value >= size) return (uint8_t)0;
				return ptr[256u * new_value + old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, glow);
		};

		if (image->modifier == 0 || image->modifier == 1) {
			uint8_t* ptr = img.player_unit_colors.at(color_index).data();
			auto player_color = [ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return ptr[new_value - 8];
				return new_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, player_color);
		} else if (image->modifier == 2 || image->modifier == 4) {
			uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
			draw_alpha(4, [color_ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			});
			uint8_t* selector = tileset_img.cloak_fade_selector.data();
			int value = image->modifier_data1;
			auto cloaking = [color_ptr, selector, value](uint8_t new_value, uint8_t old_value) {
				if (selector[new_value] <= value) return old_value;
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, cloaking);
		} else if (image->modifier == 3) {
			uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
			draw_alpha(4, [color_ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			});
		} else if (image->modifier == 8) {
			size_t data_size = data_pitch * screen_height;
			auto distortion = [data_size, dst](uint8_t new_value, uint8_t& old_value) {
				size_t offset = &old_value - dst;
				if (offset >= new_value && data_size - offset > new_value) return *(&old_value + new_value);
				return old_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, distortion);
		} else if (image->modifier == 10) {
			uint8_t* ptr = &tileset_img.dark_pcx.data[256 * 18];
			auto shadow = [ptr](uint8_t, uint8_t old_value) {
				return ptr[old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, shadow);
		} else if (image->modifier == 9) {
			draw_alpha(image->image_type->color_shift - 1, no_remap());
		} else if (image->modifier == 12) {
			if (temporary_warp_texture_buffer.size() < frame.size.x * frame.size.y) temporary_warp_texture_buffer.resize(frame.size.x * frame.size.y);
			auto& texture_frame = global_st.image_grp[(size_t)ImageTypes::IMAGEID_Warp_Texture]->frames.at(image->modifier_data1);
			draw_frame(texture_frame, false, temporary_warp_texture_buffer.data(), frame.size.x, 0, 0, frame.size.x, frame.size.y);
			draw_frame_textured(frame, temporary_warp_texture_buffer.data(), i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height);
		} else if (image->modifier == 17) {
			auto& data = tileset_img.light_pcx.at(0).data;
			uint8_t* ptr = &data.at(256u * (image->modifier_data1 - 1));
			size_t size = data.data() + data.size() - ptr;
			auto glow = [ptr, size](uint8_t, uint8_t old_value) {
				if (old_value >= size) return (uint8_t)0;
				return ptr[old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, glow);
		} else error("don't know how to draw image modifier %d", image->modifier);

	}

	// Shift+Tab: two-colour mode like the in-game toggle. The first two players render red
	// and teal (a player who is already red keeps it). Display only: game state keeps the
	// replay's own colours.
	bool two_colour_mode = false;

	int player_color(int owner) const {
		if (owner < 0 || owner >= 12) return 0;
		int own = st.players[owner].color;
		if (!two_colour_mode || owner >= 8) return own;
		std::array<int, 2> slots{-1, -1};
		int n = 0;
		for (int i = 0; i != 8 && n != 2; ++i) {
			auto& pl = st.players[i];
			if (pl.initially_active && (int)pl.race >= 0 && (int)pl.race <= 2) slots[n++] = i;
		}
		if (owner != slots[0] && owner != slots[1]) return own;
		const int red = 0, teal = 2;
		int red_player = slots[0];
		if (slots[1] >= 0 && st.players[slots[1]].color == red && st.players[slots[0]].color != red) red_player = slots[1];
		return owner == red_player ? red : teal;
	}

	a_vector<const unit_t*> current_selection_sprites_set = a_vector<const unit_t*>(2500);
	a_vector<const sprite_t*> current_selection_sprites;

	void draw_selection_circle(const sprite_t* sprite, const unit_t* u, uint8_t* data, size_t data_pitch) {
		auto* image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		xy map_pos = sprite->position + xy(0, sprite->sprite_type->selection_circle_vpos);

		auto* grp = global_st.image_grp[(size_t)image_type->id];
		auto& frame = grp->frames.at(0);

		map_pos.x += int(frame.offset.x - grp->width / 2);
		map_pos.y += int(frame.offset.y - grp->height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		size_t color_index = player_color(sprite->owner);
		uint8_t color = img.player_unit_colors.at(color_index)[0];
		if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
			color = tileset_img.resource_minimap_color;
		}
		auto player_color = [color](uint8_t new_value, uint8_t) {
			if (new_value >= 0 && new_value < 8) return color;
			return new_value;
		};
		draw_frame(frame, false, dst, data_pitch, offset_x, offset_y, width, height, player_color);

	}

	void draw_health_bars(const sprite_t* sprite, const unit_t* u, uint8_t* data, size_t data_pitch) {

		auto* selection_circle_image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		auto* selection_circle_grp = global_st.image_grp[(size_t)selection_circle_image_type->id];
		auto& selection_circle_frame = selection_circle_grp->frames.at(0);

		int offsety = sprite->sprite_type->selection_circle_vpos + selection_circle_frame.size.y / 2 + 8;

		bool has_shield = u->unit_type->has_shield;
		bool has_energy = ut_has_energy(u) || u_hallucination(u) || unit_is(u, UnitTypes::Zerg_Broodling);

		int width = sprite->sprite_type->health_bar_size;
		width -= (width - 1) % 3;
		if (width < 19) width = 19;
		int orig_width = width;
		int height = 5;
		if (has_shield) height += 2;
		if (has_energy) height += 6;

		xy map_pos = sprite->position + xy(0, offsety);

		map_pos.x += int(0 - width / 2);
		map_pos.y += int(0 - height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + width <= 0 || screen_y + height <= 0) return;

		auto filled_width = [&](int percent) {
			int r = percent * width / 100;
			if (r < 3) r = 3;
			else if (r % 3) {
				if (r % 3 > 1) r += 3 - (r % 3);
				else r -= r % 3;
			}
			return r;
		};

		int hp_percent = unit_hp_percent(u);
		int dw = filled_width(hp_percent);

		int shield_dw = 0;
		if (has_shield) {
			int shield_percent = (int)u->shield_points.integer_part() * 100 / std::max(u->unit_type->shield_points, 1);
			shield_dw = filled_width(shield_percent);
		}

		int energy_dw = 0;
		if (has_energy) {
			int energy_percent;
			if (ut_has_energy(u)) energy_percent = (int)u->energy.integer_part() * 100 / std::max((int)unit_max_energy(u).integer_part(), 1);
			else energy_percent = (int)u->remove_timer / std::max((int)default_remove_timer(u), 1);
			energy_dw = filled_width(energy_percent);
		}

		const int no_shield_colors_66[] = {18, 0, 1, 2, 18};
		const int no_shield_colors_33[] = {18, 3, 4, 5, 18};
		const int no_shield_colors_0[] = {18, 6, 7, 8, 18};
		const int no_shield_colors_bg[] = {18, 15, 16, 17, 18};

		const int with_shield_colors_66[] = {18, 0, 0, 1, 1, 2, 18};
		const int with_shield_colors_33[] = {18, 3, 3, 4, 4, 5, 18};
		const int with_shield_colors_0[] = {18, 6, 6, 7, 7, 8, 18};
		const int with_shield_colors_bg[] = {18, 15, 15, 16, 16, 17, 18};

		const int* colors_66 = has_shield ? with_shield_colors_66 : no_shield_colors_66;
		const int* colors_33 = has_shield ? with_shield_colors_33 : no_shield_colors_33;
		const int* colors_0 = has_shield ? with_shield_colors_0 : no_shield_colors_0;
		const int* colors_bg = has_shield ? with_shield_colors_bg : no_shield_colors_bg;

		int offset_x = 0;
		int offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
			dw = std::max(dw + screen_x, 0);
			shield_dw = std::max(shield_dw + screen_x, 0);
			energy_dw = std::max(energy_dw + screen_x, 0);
			width = std::max(width + screen_x, 0);
			screen_x = 0;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
			height += screen_y;
			screen_y = 0;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, (int)screen_width - screen_x);
		height = std::min(height, (int)screen_height - screen_y);

		if (dw > width) dw = width;
		if (shield_dw > width) shield_dw = width;
		if (energy_dw > width) energy_dw = width;

		int hp_height = std::min(std::max((has_shield ? 7 : 5) - offset_y, 0), height);

		for (int i = offset_y; i < offset_y + hp_height; ++i) {
			int ci = hp_percent >= 66 ? colors_66[i] : hp_percent >= 33 ? colors_33[i] : colors_0[i];
			int c = img.hp_bar_colors.at(ci);

			if (dw > 0) memset(dst, c, dw);
			if (width - dw > 0) {
				c = img.hp_bar_colors.at(colors_bg[i]);
				memset(dst + dw, c, width - dw);
			}
			dst += data_pitch;
		}

		if (has_shield) {
			const int shield_colors[] = {18, 10, 11, 18};
			const int shield_colors_bg[] = {18, 16, 17, 18};

			dst = data + screen_y * data_pitch + screen_x;

			for (int i = offset_y; i < std::min(4, height); ++i) {
				int c = img.hp_bar_colors.at(shield_colors[i]);

				if (shield_dw > 0) memset(dst, c, shield_dw);
				if (width - shield_dw > 0) {
					c = img.hp_bar_colors.at(shield_colors_bg[i]);
					memset(dst + shield_dw, c, width - shield_dw);
				}
				dst += data_pitch;
			}
		}

		int energy_offset = std::max((has_shield ? 8 : 6) - offset_y, 0);
		int energy_begin = std::max(offset_y - (has_shield ? 8 : 6), 0);
		int energy_end = std::min(5, offset_y + height - (has_shield ? 8 : 6));

		if (has_energy ) {
			dst = data + (screen_y + energy_offset) * data_pitch + screen_x;
			const int energy_colors[] = {18, 12, 13, 14, 18};
			for (int i = energy_begin; i < energy_end; ++i) {
				int c = img.hp_bar_colors.at(energy_colors[i]);

				if (energy_dw > 0) memset(dst, c, energy_dw);
				if (width - energy_dw > 0) {
					c = img.hp_bar_colors.at(no_shield_colors_bg[i]);
					memset(dst + energy_dw, c, width - energy_dw);
				}
				dst += data_pitch;
			}
		}

		dst = data + screen_y * data_pitch + screen_x;
		if (offset_x % 3) dst += 3 - offset_x % 3;

		int c = img.hp_bar_colors.at(18);
		for (int x = 0; x < orig_width; x += 3) {
			if (x < offset_x || x >= offset_x + width) continue;
			for (int y = 0; y != hp_height; ++y) {
				*dst = c;
				dst += data_pitch;
			}
			if (has_energy && energy_end > energy_begin) {
				if (energy_offset) dst += data_pitch;
				for (int i = energy_begin; i != energy_end; ++i) {
					*dst = c;
					dst += data_pitch;
				}
				if (energy_offset) dst -= data_pitch;
				dst -= (energy_end - energy_begin) * data_pitch;
			}
			dst -= hp_height * data_pitch;
			dst += 3;
		}

	}

	void draw_sprite(const sprite_t* sprite, uint8_t* data, size_t data_pitch) {
		const unit_t* draw_selection_u = current_selection_sprites_set.at(sprite->index);
		const unit_t* draw_health_bars_u = draw_selection_u ? draw_selection_u : health_bar_sprites_set.at(sprite->index);
		for (auto* image : ptr(reverse(sprite->images))) {
			if (i_flag(image, image_t::flag_hidden)) continue;
			if (draw_selection_u && image->modifier != 10) {
				if (hd_drawing) hd_draw_selection_circle(sprite, draw_selection_u);
				else draw_selection_circle(sprite, draw_selection_u, data, data_pitch);
				draw_selection_u = nullptr;
			}
			if (hd_drawing) hd_draw_image(image, player_color(sprite->owner));
			else draw_image(image, data, data_pitch, player_color(sprite->owner));
		}
		if (draw_health_bars_u && !u_invincible(draw_health_bars_u)) {
			draw_health_bars(sprite, draw_health_bars_u, data, data_pitch);
		}
	}

	a_vector<std::pair<uint32_t, const sprite_t*>> sorted_sprites;

	// 'b': health bars under every player's units, not just the selected ones (on by default,
	// like Remastered's "always show HP bars").
	bool show_all_health_bars = true;
	a_vector<const unit_t*> health_bar_sprites_set = a_vector<const unit_t*>(2500);
	a_vector<const sprite_t*> health_bar_sprites;

	void draw_sprites(uint8_t* data, size_t data_pitch) {

		image_draw_queue.clear();

		sorted_sprites.clear();

		auto screen_tile = screen_tile_bounds();

		size_t from_y = screen_tile.from.y;
		if (from_y < 4) from_y = 0;
		else from_y -= 4;
		size_t to_y = screen_tile.to.y;
		if (to_y >= game_st.map_tile_height - 4) to_y = game_st.map_tile_height - 1;
		else to_y += 4;
		for (size_t y = from_y; y != to_y; ++y) {
			for (auto* sprite : ptr(st.sprites_on_tile_line.at(y))) {
				if (s_hidden(sprite)) continue;
				sorted_sprites.emplace_back(sprite_depth_order(sprite), sprite);
			}
		}

		// Equal depths keep the game's own order (tile line, then list order). Breaking ties
		// by sprite address made overlapping translucent sprites blend in a different order
		// from run to run, as heap layout varies.
		std::stable_sort(sorted_sprites.begin(), sorted_sprites.end(), [](auto& a, auto& b) { return a.first < b.first; });

		for (auto uid : current_selection) {
			auto* u = get_unit(uid);
			if (!u) continue;
			current_selection_sprites_set.at(u->sprite->index) = u;
			current_selection_sprites.push_back(u->sprite);
		}

		if (show_all_health_bars) {
			for (int i = 0; i != 8; ++i) {
				for (unit_t* u : ptr(st.player_units[i])) {
					if (!u->sprite || s_hidden(u->sprite)) continue;
					health_bar_sprites_set.at(u->sprite->index) = u;
					health_bar_sprites.push_back(u->sprite);
				}
			}
		}

		if (hd_drawing) hd_prepare_sprites();
		for (auto& v : sorted_sprites) {
			draw_sprite(v.second, data, data_pitch);
		}

		for (auto* s : health_bar_sprites) health_bar_sprites_set.at(s->index) = nullptr;
		health_bar_sprites.clear();

		for (auto* s : current_selection_sprites) {
			current_selection_sprites_set.at(s->index) = nullptr;
		}
		current_selection_sprites.clear();
	}

	void fill_rectangle(uint8_t* data, size_t data_pitch, rect area, uint8_t index) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t pitch = data_pitch;
		size_t from_y = area.from.y;
		size_t to_y = area.to.y;
		uint8_t* ptr = data + data_pitch * from_y + area.from.x;
		for (size_t i = from_y; i != to_y; ++i) {
			memset(ptr, index, width);
			ptr += pitch;
		}
	}

	// Outlines the rectangle with corners `from` and `to` (both inclusive) in a screen-sized
	// buffer. Edges outside the screen are left out rather than drawn at its border.
	template <typename T>
	void outline_rect(T* data, size_t pitch, xy from, xy to, T value) {
		const int w = (int)screen_width, h = (int)screen_height;
		if (from.x > to.x || from.y > to.y || to.x < 0 || to.y < 0 || from.x >= w || from.y >= h) return;
		const int x0 = std::max(0, from.x), x1 = std::min(w - 1, to.x);
		const int y0 = std::max(0, from.y), y1 = std::min(h - 1, to.y);
		for (int x = x0; x <= x1; ++x) {
			if (from.y >= 0) data[(size_t)from.y * pitch + x] = value;
			if (to.y < h) data[(size_t)to.y * pitch + x] = value;
		}
		for (int y = y0; y <= y1; ++y) {
			if (from.x >= 0) data[(size_t)y * pitch + from.x] = value;
			if (to.x < w) data[(size_t)y * pitch + to.x] = value;
		}
	}

	// The outline of the pixels in `area` (`to` exclusive).
	void line_rectangle(uint8_t* data, size_t data_pitch, rect area, uint8_t index) {
		outline_rect<uint8_t>(data, data_pitch, area.from, area.to - xy(1, 1), index);
	}

	bool unit_visble_on_minimap(unit_t* u) {
		if (u->owner < 8 && u->sprite->visibility_flags == 0) return false;
		if (ut_turret(u)) return false;
		if (unit_is_trap(u)) return false;
		if (unit_is(u, UnitTypes::Spell_Dark_Swarm)) return false;
		if (unit_is(u, UnitTypes::Spell_Disruption_Web)) return false;
		return true;
	}

	// ---- Minimap: drawn in window pixels, crisp at any zoom, flush in the bottom-left ----
	// Each tile shows its megatile averaged over a 4x4 grid (the remastered tiles while those
	// are drawn), rather than one sample pixel per tile scaled up with the game view.
	enum { minimap_grid = 4 };
	std::unique_ptr<native_window_drawing::surface> minimap_surface;
	struct minimap_megatile {
		bool ready = false;
		std::array<uint32_t, minimap_grid * minimap_grid> cells{};  // hud_rgba, opaque
	};
	a_vector<minimap_megatile> minimap_cells[2];  // classic, remastered; by megatile index
	int minimap_cells_tileset = -1;
	bool minimap_hd = false;  // the frame being drawn uses the remastered art

	// Window pixels: the longer side about a quarter of the window height.
	rect minimap_window_rect() const {
		int tw = (int)game_st.map_tile_width, th = (int)game_st.map_tile_height;
		if (tw <= 0 || th <= 0 || window_height <= 0 || viewport_w <= 0) return {};
		int side = std::max(128, std::min(384, (int)(window_height * 0.26)));
		side = std::min(side, std::min(viewport_w, window_height) / 2);
		int w = std::max(1, side * tw / std::max(tw, th)), h = std::max(1, side * th / std::max(tw, th));
		return rect{{viewport_x, window_height - h}, {viewport_x + w, window_height}};
	}

	// The same rect in logical (pre-zoom) coordinates, for mouse hit tests.
	rect get_minimap_area() {
		rect r = minimap_window_rect();
		if (r.to.x <= r.from.x || zoom <= 0) return {};
		rect a;
		a.from = {(int)((r.from.x - viewport_x) / zoom), (int)(r.from.y / zoom)};
		a.to = {(int)std::ceil((r.to.x - viewport_x) / zoom), (int)std::ceil(r.to.y / zoom)};
		return a;
	}

	size_t minimap_tile_megatile(size_t ti) const {
		if (st.tiles[ti].flags & tile_t::flag_has_creep) return game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[ti]];
		return st.tiles_mega_tile_index[ti];
	}

	const minimap_megatile& minimap_megatile_cells(size_t index) {
		if (minimap_cells_tileset != (int)game_st.tileset_index) {
			for (auto& c : minimap_cells) c.clear();
			minimap_cells_tileset = (int)game_st.tileset_index;
		}
		auto& cache = minimap_cells[minimap_hd ? 1 : 0];
		if (cache.size() <= index) cache.resize(index + 1);
		auto& m = cache[index];
		if (m.ready) return m;
		m.ready = true;
#ifdef OPENBW_REMASTERED
		if (minimap_hd && hd) {
			std::vector<uint32_t> v;
			if (hd->megatile_cells(index, minimap_grid, v) && v.size() == m.cells.size()) {
				for (size_t i = 0; i != v.size(); ++i) m.cells[i] = v[i] | 0xff000000u;
				return m;
			}
		}
#endif
		// Classic: each cell is one 8x8 minitile, averaged through the tileset palette.
		if (index >= tileset_img.vx4.size() || tileset_img.wpe.size() != 256 * 4) return m;
		auto& images = tileset_img.vx4[index].images;
		const size_t word = sizeof(vr4_entry::bitmap_t);
		for (size_t c = 0; c != 16; ++c) {
			size_t vi = images[c] / 2;
			if (vi >= tileset_img.vr4.size()) continue;
			auto& bm = tileset_img.vr4[vi].bitmap;
			unsigned r = 0, g = 0, b = 0;
			for (size_t i = 0; i != 64; ++i) {
				uint8_t ci = (uint8_t)(bm[i / word] >> (8 * (i % word)));
				r += tileset_img.wpe[4 * ci];
				g += tileset_img.wpe[4 * ci + 1];
				b += tileset_img.wpe[4 * ci + 2];
			}
			m.cells[c] = hud_rgba(r / 64, g / 64, b / 64, 255);
		}
		return m;
	}

	uint32_t palette_rgba(int index) const {
		if (tileset_img.wpe.size() != 256 * 4) return hud_rgba(255, 255, 255, 255);
		index &= 0xff;
		return hud_rgba(tileset_img.wpe[4 * index], tileset_img.wpe[4 * index + 1], tileset_img.wpe[4 * index + 2], 255);
	}

	void draw_minimap_window() {
		rect r = minimap_window_rect();
		const int w = r.to.x - r.from.x, h = r.to.y - r.from.y;
		if (w <= 0 || h <= 0) return;
		if (!minimap_surface || minimap_surface->w != w || minimap_surface->h != h) {
			minimap_surface = native_window_drawing::create_rgba_surface(w, h);
			minimap_surface->set_blend_mode(native_window_drawing::blend_mode::none);
		}
		uint32_t* buf = (uint32_t*)minimap_surface->lock();
		const size_t pitch = minimap_surface->pitch / 4;
		const int tw = (int)game_st.map_tile_width, th = (int)game_st.map_tile_height;
		const uint8_t fog_mask = show_fog ? fog_vision_mask() : 0;

		// Per column / row: the tile, the cell within it, and the position between tile
		// centres for the fog's interpolation (as draw_fog does it on the map).
		struct axis { int tile, cell, fog_tile; double fog_frac; };
		auto make_axis = [&](int n, int tiles) {
			a_vector<axis> v((size_t)n);
			for (int i = 0; i != n; ++i) {
				double f = (i + 0.5) * tiles / n;
				int t = std::min(tiles - 1, (int)f);
				double c = f - 0.5;
				int ct = (int)std::floor(c);
				v[i] = {t, std::min((int)minimap_grid - 1, (int)((f - t) * minimap_grid)), ct, c - ct};
			}
			return v;
		};
		auto ax = make_axis(w, tw), ay = make_axis(h, th);
		for (int y = 0; y != h; ++y) {
			uint32_t* row = buf + (size_t)y * pitch;
			const axis& yv = ay[y];
			for (int x = 0; x != w; ++x) {
				const axis& xv = ax[x];
				size_t ti = (size_t)yv.tile * tw + xv.tile;
				uint32_t c = minimap_megatile_cells(minimap_tile_megatile(ti)).cells[yv.cell * minimap_grid + xv.cell];
				if (fog_mask) {
					double a = fog_tile_level(xv.fog_tile, yv.fog_tile, fog_mask), b = fog_tile_level(xv.fog_tile + 1, yv.fog_tile, fog_mask);
					double cc = fog_tile_level(xv.fog_tile, yv.fog_tile + 1, fog_mask), d = fog_tile_level(xv.fog_tile + 1, yv.fog_tile + 1, fog_mask);
					double top = a + (b - a) * xv.fog_frac, bottom = cc + (d - cc) * xv.fog_frac;
					unsigned k = (unsigned)((top + (bottom - top) * yv.fog_frac) * 256 / 31);
					if (k < 256) c = hud_rgba((c & 0xff) * k >> 8, (c >> 8 & 0xff) * k >> 8, (c >> 16 & 0xff) * k >> 8, 255);
				}
				row[x] = c;
			}
		}

		auto fill = [&](double x0, double y0, double x1, double y1, uint32_t c) {
			int ix0 = std::max(0, (int)std::floor(x0)), iy0 = std::max(0, (int)std::floor(y0));
			int ix1 = std::min(w, std::max(ix0 + 1, (int)std::floor(x1))), iy1 = std::min(h, std::max(iy0 + 1, (int)std::floor(y1)));
			for (int y = iy0; y < iy1; ++y) std::fill(buf + (size_t)y * pitch + ix0, buf + (size_t)y * pitch + std::max(ix0, ix1), c);
		};
		const double sx = (double)w / tw, sy = (double)h / th;
		for (size_t i = 12; i != 0;) {
			--i;
			for (unit_t* u : ptr(st.player_units[i])) {
				if (!unit_visble_on_minimap(u)) continue;
				// Under fog, neutral units (resources, critters) show only where explored.
				if (fog_mask && u->owner >= 8 && fog_tile_level(u->sprite->position.x / 32, u->sprite->position.y / 32, fog_mask) == 0) continue;
				int color = img.player_minimap_colors.at(player_color(u->owner));
				if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) color = tileset_img.resource_minimap_color;
				int tiles_w = u->unit_type->placement_size.x / 32, tiles_h = u->unit_type->placement_size.y / 32;
				if (ut_building(u)) {
					tiles_w = std::min(tiles_w, 4);
					tiles_h = std::min(tiles_h, 4);
				}
				tiles_w = std::max(tiles_w, 2);
				tiles_h = std::max(tiles_h, 2);
				xy from = (u->sprite->position - u->unit_type->placement_size / 2) / 32u;
				fill(from.x * sx, from.y * sy, (from.x + tiles_w) * sx, (from.y + tiles_h) * sy, palette_rgba(color));
			}
		}

		// Camera: a white outline, clipped to the minimap.
		{
			int x0 = (int)std::floor(screen_pos.x / 32.0 * sx), y0 = (int)std::floor(screen_pos.y / 32.0 * sy);
			int x1 = (int)std::ceil((screen_pos.x + view_width) / 32.0 * sx) - 1, y1 = (int)std::ceil((screen_pos.y + view_height) / 32.0 * sy) - 1;
			x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(w - 1, x1), y1 = std::min(h - 1, y1);
			uint32_t white = hud_rgba(255, 255, 255, 255);
			if (x0 <= x1 && y0 <= y1) {
				for (int x = x0; x <= x1; ++x) buf[(size_t)y0 * pitch + x] = buf[(size_t)y1 * pitch + x] = white;
				for (int y = y0; y <= y1; ++y) buf[(size_t)y * pitch + x0] = buf[(size_t)y * pitch + x1] = white;
			}
		}
		// Edge on the top and right (the other two are the window's).
		for (int x = 0; x != w; ++x) buf[x] = hud_rgba(0, 0, 0, 255);
		for (int y = 0; y != h; ++y) buf[(size_t)y * pitch + w - 1] = hud_rgba(0, 0, 0, 255);
		minimap_surface->unlock();
		minimap_surface->blit(&*window_surface, r.from.x, r.from.y);
	}

	// ---- Fog of war ('f' with the graphs closed) ----
	// The combined vision of every player in the game, drawn like Brood War's: dark.pcx
	// gives 32 darkness levels per palette colour; tiles some player sees now are full
	// brightness, explored ones are dimmed, unexplored ones black, interpolated between
	// tile centres so the edges are soft.
	bool show_fog = false;
	static constexpr int fog_level_explored = 15;  // of 31 (dark.pcx row 31 is identity)

	uint8_t fog_vision_mask() const {
		uint8_t mask = 0;
		for (int i = 0; i != 8; ++i) if (st.players[i].initially_active) mask |= 1 << i;
		return mask;
	}

	int fog_tile_level(int tx, int ty, uint8_t mask) const {
		tx = std::max(0, std::min((int)game_st.map_tile_width - 1, tx));
		ty = std::max(0, std::min((int)game_st.map_tile_height - 1, ty));
		auto& tile = st.tiles[(size_t)ty * game_st.map_tile_width + tx];
		if ((uint8_t)~tile.visible & mask) return 31;
		if ((uint8_t)~tile.explored & mask) return fog_level_explored;
		return 0;
	}

	void draw_fog(uint8_t* data, size_t data_pitch) {
		if (!show_fog) return;
		uint8_t mask = fog_vision_mask();
		if (!mask) return;
		const uint8_t* dark = tileset_img.dark_pcx.data.data();
		// Cell (cx, cy) spans world pixels between the centres of tiles cx..cx+1, cy..cy+1.
		int cx_from = (screen_pos.x + 16) / 32 - 1, cx_to = (screen_pos.x + (int)screen_width - 16) / 32;
		int cy_from = (screen_pos.y + 16) / 32 - 1, cy_to = (screen_pos.y + (int)screen_height - 16) / 32;
		for (int cy = cy_from; cy <= cy_to; ++cy) {
			for (int cx = cx_from; cx <= cx_to; ++cx) {
				int a = fog_tile_level(cx, cy, mask), b = fog_tile_level(cx + 1, cy, mask);
				int c = fog_tile_level(cx, cy + 1, mask), d = fog_tile_level(cx + 1, cy + 1, mask);
				if ((a & b & c & d) == 31) continue;
				int wx0 = cx * 32 + 16, wy0 = cy * 32 + 16;
				int sx0 = std::max(0, wx0 - screen_pos.x), sx1 = std::min((int)screen_width, wx0 + 32 - screen_pos.x);
				int sy0 = std::max(0, wy0 - screen_pos.y), sy1 = std::min((int)screen_height, wy0 + 32 - screen_pos.y);
				for (int sy = sy0; sy < sy1; ++sy) {
					int v = sy + screen_pos.y - wy0;
					int left = a * (32 - v) + c * v, right = b * (32 - v) + d * v;  // x32
					uint8_t* row = data + (size_t)sy * data_pitch;
					for (int sx = sx0; sx < sx1; ++sx) {
						int u = sx + screen_pos.x - wx0;
						int level = (left * (32 - u) + right * u) >> 10;
						row[sx] = dark[level * 256 + row[sx]];
					}
				}
			}
		}
	}


	int replay_frame = 0;

	// The replay bar lives in window pixels (drawn crisp after scaling, see draw_replay_bar);
	// this is its bottom-right placement there.
	rect replay_bar_window_rect() const {
		// The whole panel, buttons included, takes about a third of the view's width.
		int w = std::max(160, std::min(440, viewport_w / 3) - (int)replay_controls_w);
		int h = 14;
		int x1 = viewport_x + viewport_w - 16, y1 = window_height - 14;
		return rect{{x1 - w, y1 - h}, {x1, y1}};
	}

	// Same rect in logical (pre-zoom) coordinates, for mouse hit tests.
	rect get_replay_slider_area() {
#ifdef EMSCRIPTEN
		return {};
#endif
		if (window_height <= 0 || viewport_w <= 0) return {};
		rect r = replay_bar_window_rect();
		r.from.x = (int)((r.from.x - viewport_x) / zoom);
		r.to.x = (int)((r.to.x - viewport_x) / zoom);
		r.from.y = (int)(r.from.y / zoom);
		r.to.y = (int)(r.to.y / zoom);
		if (r.from.x < 0 || r.from.y < 0) return {};
		return r;
	}

	void draw_ui(uint8_t* data, size_t data_pitch) {
		// The replay bar is drawn crisp in window pixels by draw_replay_bar().
		(void)data;
		(void)data_pitch;
	}

	virtual void draw_callback(uint8_t* data, size_t data_pitch) {
	}

	a_vector<const image_t*> image_draw_queue;

	bool use_new_images = false;

	bool new_images_index_loaded = false;

	a_vector<char> grp_new_image_state = a_vector<char>((size_t)ImageTypes::None);
	a_unordered_set<a_string> new_images_index;
	string_table_data images_tbl;

	a_vector<a_vector<std::unique_ptr<native_window_drawing::surface>>> new_images;
	a_vector<a_vector<std::unique_ptr<native_window_drawing::surface>>> new_images_flipped;

	bool is_new_image(const image_t* image) {
		if (!use_new_images) return false;
		if (!new_images_index_loaded) {
			new_images_index_loaded = true;
			async_read_file("index.txt", [this](const uint8_t* data, size_t len) {
				if (!data) {
					ui::log("failed to load index.txt :(\n");
					return;
				}
				char* c = (char*)data;
				char* e = (char*)data + len;
				while (c != e && (*c == '\r' || *c =='\n' || *c == ' ')) ++c;
				while (c != e) {
					char* s = c;
					while (c != e && *c != '\r' && *c !='\n' && *c != ' ') ++c;
					a_string fn(s, c - s);
					while (c != e && (*c == '\r' || *c =='\n' || *c == ' ')) ++c;
					for (char& c : fn) {
						if (c == '\\') c = '/';
					}
					if (!fn.empty() && fn.front() == '/') fn.erase(fn.begin());
					if (!fn.empty() && fn.back() == '/') fn.erase(std::prev(fn.end()));
					ui::log("index entry '%s'\n", fn);
					new_images_index.insert(std::move(fn));
				}
			});
		}
		if (new_images_index.empty()) return false;
		size_t index = image->grp - global_st.grps.data();
		auto& state = grp_new_image_state.at(index);
		if (state == 0) {
			state = 2;
			a_string fn = images_tbl.at(image->image_type->grp_filename_index);
			for (char& c : fn) {
				if (c == '\\') c = '/';
			}
			for (auto i = fn.rbegin(); i != fn.rend(); ++i) {
				if (*i == '.') {
					fn.erase(std::prev(i.base()), fn.end());
				}
				if (*i == '/') break;
			}
			if (!fn.empty() && fn.front() == '/') fn.erase(fn.begin());
			fn = "unit/" + fn;
			ui::log("checking '%s' (image %d, grp %u)\n", fn, (int)image->image_type->id, index);
			if (new_images_index.count(fn)) {
				size_t frames = image->grp->frames.size();
				if (new_images.size() <= index) new_images.resize(index + 1);
				new_images[index].resize(frames);
				if (new_images_flipped.size() <= index) new_images_flipped.resize(index + 1);
				new_images_flipped[index].resize(frames);
				ui::log("loading %d frames...\n", frames);
				auto frames_left = std::make_shared<size_t>(frames);
				for (size_t i = 0; i != frames; ++i) {
					a_string frame_fn = format("%s/%02u.png", fn, i);
					async_read_file(frame_fn, [this, frame_fn, index, i, frames_left](const uint8_t* data, size_t len) {
						if (!data) {
							ui::log("failed to load '%s'\n", frame_fn);
							return;
						}
						new_images.at(index).at(i) = native_window_drawing::load_image(data, len);
						--*frames_left;
						if (*frames_left == 0) {
							grp_new_image_state.at(index) = 1;

							ui::log("grp %d successfully loaded %d frames\n", index, new_images.at(index).size());
						}
					});
				}
			}
		}
		//ui::log("index %d state %d\n", index, state);
		return state == 1;
	}

	native_window_drawing::surface* get_new_image_surface(const image_t* image, bool flipped) {
		size_t index = image->grp - global_st.grps.data();
		size_t frame = image->frame_index;
		if (flipped) {
			auto& r = new_images_flipped.at(index).at(frame);
			if (!r) {
				auto* s = new_images.at(index).at(frame).get();
				if (!s) return nullptr;
				r = flip_image(s);
			}
			return r.get();
		} else {
			return new_images.at(index).at(frame).get();
		}
	}

	std::unique_ptr<native_window_drawing::surface> tmp_surface;

	void draw_new_image(const image_t* image) {
		xy map_pos = get_image_center_map_position(image);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		auto* surface = get_new_image_surface(image, i_flag(image, image_t::flag_horizontally_flipped));
		if (!surface) {
			ui::log("ERROR: new image %d (grp %d) frame %d does not exist\n", (int)image->image_type->id, image->grp - global_st.grps.data(), image->frame_index);
			return;
		}

		auto scale = 114_fp8;

		size_t w = (fp8::integer(surface->w) * scale).integer_part();
		size_t h = (fp8::integer(surface->w) * scale).integer_part();

		size_t orig_w = w;
		size_t orig_h = h;

		screen_x -= w / 2;
		screen_y -= w / 2;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + (int)w <= 0 || screen_y + (int)h <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
			w += screen_x;
			screen_x = 0;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
			h += screen_y;
			screen_y = 0;
		}

		w = std::min(w, screen_width - screen_x);
		h = std::min(h, screen_height - screen_y);

		if (image->modifier == 10) {
			if (!tmp_surface || (size_t)tmp_surface->w < orig_w || (size_t)tmp_surface->h < orig_h) {
				tmp_surface = native_window_drawing::create_rgba_surface(orig_w, orig_h);
			}
			surface->set_blend_mode(native_window_drawing::blend_mode::none);
			surface->blit_scaled(&*tmp_surface, 0, 0, orig_w, orig_h);

			size_t src_pitch = tmp_surface->pitch / 4;
			size_t dst_pitch = rgba_surface->pitch / 4;
			uint32_t* src = (uint32_t*)tmp_surface->lock();
			uint32_t* dst = (uint32_t*)rgba_surface->lock();

			src += src_pitch * offset_y + offset_x;
			dst += dst_pitch * screen_y + screen_x;

			size_t src_skip = src_pitch - w;
			size_t dst_skip = dst_pitch - w;

			for (size_t y = h; y--;) {

				for (size_t x = w; x--;) {
					uint32_t s = *src;
					uint32_t d = *dst;
					if (s >> 24 >= 16) *dst = (d & 0xfefefe) / 2 | 0xff000000;
					++src;
					++dst;
				}

				src += src_skip;
				dst += dst_skip;
			}

			tmp_surface->unlock();
			rgba_surface->unlock();

		} else {
			surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
			surface->blit_scaled(&*rgba_surface, screen_x - offset_x, screen_y - offset_y, orig_w, orig_h);
		}
	}

	void draw_image_queue() {
		for (auto* image : image_draw_queue) {
			draw_new_image(image);
		}
	}

	// ---- Remastered graphics (F2) ----
	// With a StarCraft: Remastered install available, the map and sprites are drawn from its
	// HD assets (scr_render.h) straight into a window-resolution canvas; health bars, the
	// minimap and draw_callback() still draw into the indexed buffer, which is then laid over
	// the canvas with a sentinel index as transparency. Without an install (or with the
	// setting off) the classic indexed path runs unchanged.
	bool use_remastered = true;
	bool hd_drawing = false;  // inside draw_sprites() for a remastered frame
#ifdef OPENBW_REMASTERED
	std::unique_ptr<scr::graphics> hd;
	bool hd_tried = false;
	std::unique_ptr<native_window_drawing::surface> hd_surface;
	a_vector<uint8_t> hd_shadow;  // all zero between frames (resolve_shadows clears what it uses)
	bool hd_shadow_clean = false;
	scr::box hd_shadow_area;
	scr::draw_list hd_list;  // the frame's map and sprite draws, run by hd_flush()
	scr::image hd_black_tile;  // drawn (in order, through hd_list) for a megatile that fails to decode
	scr::canvas hd_canvas;
	int hd_cam_x = 0;
	int hd_cam_y = 0;

	// $OPENBW_REMASTERED_DIR, else the platform's default install location. The folder is
	// the one holding .build.info (e.g. ".../Program Files (x86)/StarCraft").
	static a_string remastered_install_dir() {
		auto exists = [](const a_string& dir) {
			FILE* f = std::fopen((dir + "/.build.info").c_str(), "rb");
			if (f) std::fclose(f);
			return f != nullptr;
		};
		const char* env = std::getenv("OPENBW_REMASTERED_DIR");
		if (env && *env) return env;
		a_vector<a_string> candidates;
#ifdef _WIN32
		candidates.push_back("C:/Program Files (x86)/StarCraft");
#elif defined(__APPLE__)
		candidates.push_back("/Applications/StarCraft");
#else
		// Wine's default prefix, and Lutris's default Battle.net prefix.
		const char* home = std::getenv("HOME");
		if (home && *home) {
			candidates.push_back(a_string(home) + "/.wine/drive_c/Program Files (x86)/StarCraft");
			candidates.push_back(a_string(home) + "/Games/battlenet/drive_c/Program Files (x86)/StarCraft");
		}
#endif
		for (auto& c : candidates) if (exists(c)) return c;
		return {};
	}

	bool hd_ready() {
		if (!use_remastered || !wnd) {
			if (hd) hd->release();  // classic graphics: free the decoded art
			return false;
		}
		if (!hd_tried) {
			hd_tried = true;
			a_string dir = remastered_install_dir();
			if (dir.empty()) {
				ui::log("remastered graphics: no install found (set OPENBW_REMASTERED_DIR)\n");
			} else {
				hd = std::make_unique<scr::graphics>();
				std::string error;
				if (!hd->open(dir.c_str(), error)) {
					ui::log("remastered graphics: %s: %s\n", dir, error.c_str());
					hd.reset();
				} else {
					ui::log("remastered graphics from %s\n", dir);
				}
			}
		}
		if (!hd) return false;
		hd->set_zoom(hd_zoom());  // may switch HD/HD2 sets, so before the tileset check
		static const char* names[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};
		return hd->set_tileset(names[game_st.tileset_index % 8]);
	}

	// The HD canvas scale: the zoom rounded to whole pixels per 32 px tile, so terrain,
	// sprites and fog share one grid (equal to `zoom` at every zoom step).
	double hd_zoom() const { return std::max(1L, std::lround(32 * zoom)) / 32.0; }

	uint32_t hd_palette_rgb(size_t index) const {
		return scr::rgba(tileset_img.wpe[4 * index], tileset_img.wpe[4 * index + 1], tileset_img.wpe[4 * index + 2], 255);
	}

	// Team colour: the player's minimap colour, as Remastered uses.
	uint32_t hd_team_rgb(size_t color_index) const {
		return hd_palette_rgb(img.player_minimap_colors.at(color_index));
	}

	int hd_screen_x(double map_x) const { return (int)std::lround(map_x * hd_zoom()) - hd_cam_x; }
	int hd_screen_y(double map_y) const { return (int)std::lround(map_y * hd_zoom()) - hd_cam_y; }

	void hd_begin_frame() {
		int w = viewport_w, h = window_height;
		if (!hd_surface || hd_surface->w != w || hd_surface->h != h) {
			hd_surface = native_window_drawing::create_rgba_surface(w, h);
			hd_surface->set_blend_mode(native_window_drawing::blend_mode::none);
		}
		// A frame that did not reach hd_flush() may have left coverage behind.
		if (hd_shadow.size() != (size_t)w * h || !hd_shadow_clean) hd_shadow.assign((size_t)w * h, 0);
		hd_shadow_clean = false;
		hd_shadow_area = {};
		hd_list.clear();
		hd_canvas.px = (uint32_t*)hd_surface->lock();
		hd_canvas.pitch = hd_surface->pitch / 4;
		hd_canvas.w = w;
		hd_canvas.h = h;
		hd_canvas.shadow = hd_shadow.data();
		hd_canvas.shadow_area = &hd_shadow_area;
		hd_cam_x = (int)std::lround(screen_pos.x * hd_zoom());
		hd_cam_y = (int)std::lround(screen_pos.y * hd_zoom());
	}

	// Draws the recorded map and sprites (in parallel bands), then applies their shadows.
	void hd_flush() {
		hd_list.run(hd_canvas);
		scr::resolve_shadows(hd_canvas);
		hd_shadow_clean = true;
	}

	void hd_fill_black(int x0, int y0, int x1, int y1) {
		auto& c = hd_canvas;
		x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, c.w), y1 = std::min(y1, c.h);
		for (int y = y0; y < y1; ++y) {
			if (x0 < x1) std::fill_n(c.px + (size_t)y * c.pitch + x0, x1 - x0, scr::rgba(0, 0, 0, 255));
		}
	}

	void hd_end_frame() {
		hd_surface->unlock();
		hd_canvas = {};
		hd->end_frame();
	}

	// The megatile drawn for map tile `ti` (creep tiles use a random creep megatile).
	size_t hd_tile_megatile(size_t ti) const {
		if (st.tiles[ti].flags & tile_t::flag_has_creep) return game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[ti]];
		return st.tiles_mega_tile_index[ti];
	}

	// Art wanted for the frame being drawn, gathered so that whatever the current zoom lacks
	// (after a zoom step, most of the screen) is decoded and scaled in parallel up front
	// rather than one item at a time while drawing.
	std::vector<size_t> hd_wanted_tiles, hd_wanted_images;
	std::vector<std::pair<size_t, size_t>> hd_wanted_frames;

	void hd_draw_tiles() {
		auto& c = hd_canvas;
		int tp = hd->tile_pixels();
		int tx0 = std::max(0, hd_cam_x / tp), ty0 = std::max(0, hd_cam_y / tp);
		int tx1 = std::min((int)game_st.map_tile_width, (hd_cam_x + c.w + tp - 1) / tp);
		int ty1 = std::min((int)game_st.map_tile_height, (hd_cam_y + c.h + tp - 1) / tp);
		hd_wanted_tiles.clear();
		for (int ty = ty0; ty < ty1; ++ty) {
			for (int tx = tx0; tx < tx1; ++tx) hd_wanted_tiles.push_back(hd_tile_megatile((size_t)ty * game_st.map_tile_width + tx));
		}
		hd->prepare_megatiles(hd_wanted_tiles);
		// Black where no tile is drawn: past the map's edges (a map smaller than the view).
		// Nothing recorded so far reaches there, so filling now rather than in order is safe.
		int mx0 = tx0 * tp - hd_cam_x, my0 = ty0 * tp - hd_cam_y, mx1 = tx1 * tp - hd_cam_x, my1 = ty1 * tp - hd_cam_y;
		hd_fill_black(0, 0, c.w, my0);
		hd_fill_black(0, my1, c.w, c.h);
		hd_fill_black(0, my0, mx0, my1);
		hd_fill_black(mx1, my0, c.w, my1);
		static const xy dirs[9] = {{1, 1}, {0, 1}, {-1, 1}, {1, 0}, {-1, 0}, {1, -1}, {0, -1}, {-1, -1}, {0, 0}};
		for (int ty = ty0; ty < ty1; ++ty) {
			for (int tx = tx0; tx < tx1; ++tx) {
				size_t ti = (size_t)ty * game_st.map_tile_width + tx;
				auto& tile = st.tiles[ti];
				int left = tx * tp - hd_cam_x, top = ty * tp - hd_cam_y;
				if (auto* m = hd->megatile(hd_tile_megatile(ti))) {
					hd_list.opaque(c, *m, left, top);
				} else {
					if (hd_black_tile.w != tp) {
						hd_black_tile.w = hd_black_tile.h = tp;
						hd_black_tile.px.assign((size_t)tp * tp, scr::rgba(0, 0, 0, 255));
					}
					hd_list.opaque(c, hd_black_tile, left, top);
				}
				if (tile.flags & tile_t::flag_has_creep) continue;
				size_t creep_index = 0;
				for (size_t i = 0; i != 9; ++i) {
					size_t nx = tx + dirs[i].x, ny = ty + dirs[i].y;
					if (nx >= game_st.map_tile_width || ny >= game_st.map_tile_height) continue;
					if (st.tiles[nx + ny * game_st.map_tile_width].flags & tile_t::flag_has_creep) creep_index |= 1 << i;
				}
				size_t creep_frame = img.creep_edge_frame_index[creep_index];
				if (!creep_frame) continue;
				auto& sd = tileset_img.creep_grp.frames.at(creep_frame - 1);
				int x = hd_screen_x(tx * 32.0 + sd.offset.x), y = hd_screen_y(ty * 32.0 + sd.offset.y);
				int w = (int)std::lround(sd.size.x * hd_zoom()), h = (int)std::lround(sd.size.y * hd_zoom());
				if (auto* e = hd->creep_edge(creep_frame - 1, w, h)) hd_list.sprite(c, *e, x, y, false, scr::blend::normal, 0, 255);
			}
		}
	}

	// Where an image is drawn from: its map position (for a y-frozen image, the centre that
	// keeps its frame's top where the classic path pins it) and whether it is mirrored.
	struct hd_anchor {
		double x, y;
		bool flipped;
	};
	hd_anchor hd_image_anchor(const image_t* image) const {
		xy centre = get_image_center_map_position(image);
		hd_anchor a{(double)centre.x, (double)centre.y, i_flag(image, image_t::flag_horizontally_flipped)};
		if (i_flag(image, image_t::flag_y_frozen)) {
			auto& sd = image->grp->frames.at(image->frame_index);
			a.y = image->frozen_y_value - ((int)sd.offset.y - (int)(image->grp->height / 2));
		}
		return a;
	}

	// draw_sprites() visits whole map-width rows, so first drop images far from the screen
	// without loading their HD art: the HD frames reach at most 66 game pixels past the SD
	// grp box (measured over every image; images.dat 578 is the worst), so twice that is safe.
	bool hd_image_near_screen(const image_t* image, const hd_anchor& a) const {
		auto& c = hd_canvas;
		int mx = (int)image->grp->width / 2 + 132, my = (int)image->grp->height / 2 + 132;
		if (hd_screen_x(a.x + mx) < 0 || hd_screen_x(a.x - mx) > c.w) return false;
		if (hd_screen_y(a.y + my) < 0 || hd_screen_y(a.y - my) > c.h) return false;
		return true;
	}

	// Whether any of the image's HD frames reach the screen; loads its atlas (not a frame).
	bool hd_image_on_screen(const image_t* image, const hd_anchor& a) {
		auto& c = hd_canvas;
		auto* b = hd->bounds((size_t)image->image_type->id);
		if (!b) return false;
		double left = a.flipped ? -b->right : b->left, right = a.flipped ? -b->left : b->right;
		if (hd_screen_x(a.x + right) < -2 || hd_screen_x(a.x + left) > c.w + 2) return false;
		if (hd_screen_y(a.y + b->bottom) < -2 || hd_screen_y(a.y + b->top) > c.h + 2) return false;
		return true;
	}

	size_t hd_selection_circle_image(const sprite_t* sprite) const {
		return (size_t)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle;
	}

	// Loads and scales, in parallel, what draw_sprites() is about to draw (with the same
	// culling); anything this misses is still scaled when drawn.
	void hd_prepare_sprites() {
		auto each_image = [&](auto&& f) {
			for (auto& v : sorted_sprites) {
				for (auto* image : ptr(v.second->images)) {
					if (!i_flag(image, image_t::flag_hidden)) f(image, hd_image_anchor(image));
				}
			}
		};
		hd_wanted_images.clear();
		each_image([&](const image_t* image, const hd_anchor& a) {
			if (hd_image_near_screen(image, a)) hd_wanted_images.push_back((size_t)image->image_type->id);
		});
		hd->load(hd_wanted_images);
		hd_wanted_frames.clear();
		each_image([&](const image_t* image, const hd_anchor& a) {
			if (!hd_image_near_screen(image, a) || !hd_image_on_screen(image, a)) return;
			hd_wanted_frames.emplace_back((size_t)image->image_type->id, (size_t)image->frame_index);
			if (image->modifier == 12) hd_wanted_frames.emplace_back((size_t)ImageTypes::IMAGEID_Warp_Texture, (size_t)image->modifier_data1);
		});
		for (auto* s : current_selection_sprites) hd_wanted_frames.emplace_back(hd_selection_circle_image(s), 0);
		hd->prepare(hd_wanted_frames);
	}

	void hd_draw_image(const image_t* image, size_t color_index) {
		auto& c = hd_canvas;
		hd_anchor a = hd_image_anchor(image);
		if (!hd_image_near_screen(image, a) || !hd_image_on_screen(image, a)) return;
		bool flipped = a.flipped;
		auto* f = hd->sprite((size_t)image->image_type->id, image->frame_index);
		if (!f || f->px.empty()) return;
		int x = hd_screen_x(a.x + (flipped ? f->left_flipped : f->left));
		int y = hd_screen_y(a.y + f->top);
		if (x >= c.w || y >= c.h || x + f->px.w <= 0 || y + f->px.h <= 0) return;
		uint32_t team = hd_team_rgb(color_index);
		switch (image->modifier) {
		case 2:
		case 4:  // cloaking / decloaking: fade towards the cloaked look over modifier_data1 0..8
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::normal, team, 255 - std::min(8, (int)image->modifier_data1) * 15);
			break;
		case 3:  // cloaked, drawn for whoever can see it
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::normal, team, 135);
			break;
		case 8:  // distortion (classic: refracts what is underneath)
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::normal, team, 110);
			break;
		case 9:  // remapped: fire, smoke, glows, explosions. The classic path remaps through
			// ofire/gfire/bfire/bexpl/trans50 tables; the HD textures carry real alpha (dark
			// smoke included), so they are alpha blended, with trans50 kept at half strength.
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::normal, team, image->image_type->color_shift == 5 ? 128 : 255);
			break;
		case 10:
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::shadow, team, 255);
			break;
		case 12:  // Protoss warp-in: the warp texture through the sprite's shape
			if (auto* t = hd->sprite((size_t)ImageTypes::IMAGEID_Warp_Texture, image->modifier_data1)) {
				hd_list.mapped(c, *f, *t, x, y, flipped);
			}
			break;
		case 17:  // warp flash: brightens what is underneath, rising over modifier_data1 48..63
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::brighten, team, std::min(255, std::max(0, ((int)image->modifier_data1 - 47) * 16)));
			break;
		default:
			hd_list.sprite(c, *f, x, y, flipped, scr::blend::normal, team, 255);
			break;
		}
	}

	void hd_draw_selection_circle(const sprite_t* sprite, const unit_t* u) {
		auto* f = hd->sprite(hd_selection_circle_image(sprite), 0);
		if (!f) return;
		xy centre = sprite->position + xy(0, sprite->sprite_type->selection_circle_vpos);
		uint32_t colour = hd_team_rgb(player_color(sprite->owner));
		if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
			colour = hd_palette_rgb(tileset_img.resource_minimap_color);
		}
		hd_list.sprite(hd_canvas, *f, hd_screen_x(centre.x + f->left), hd_screen_y(centre.y + f->top), false, scr::blend::tinted, colour, 255);
	}

	// Same vision and levels as draw_fog(), interpolated per window pixel: the level (0 dark
	// .. 31 clear) is bilinear between tile centres. Fixed point, rows in parallel.
	void hd_draw_fog() {
		if (!show_fog) return;
		uint8_t mask = fog_vision_mask();
		if (!mask) return;
		auto& c = hd_canvas;
		const int tp = hd->tile_pixels();
		// Tile-centre grid coordinate of window pixel p (camera offset `cam`): (p + cam + 0.5)
		// / tp - 0.5, as tile index and 1/256 fraction.
		auto grid = [&](int p, int cam, int& cell, int& frac) {
			double g = (p + cam + 0.5) / tp - 0.5;
			cell = (int)std::floor(g);
			frac = std::min(256, (int)((g - cell) * 256.0 + 0.5));
		};
		int gx0, gy0, gx1, gy1, f;
		grid(0, hd_cam_x, gx0, f);
		grid(0, hd_cam_y, gy0, f);
		grid(c.w - 1, hd_cam_x, gx1, f);
		grid(c.h - 1, hd_cam_y, gy1, f);
		const int gw = gx1 - gx0 + 2, gh = gy1 - gy0 + 2;
		hd_fog_levels.resize((size_t)gw * gh);
		for (int y = 0; y != gh; ++y) {
			for (int x = 0; x != gw; ++x) hd_fog_levels[(size_t)y * gw + x] = (uint8_t)fog_tile_level(gx0 + x, gy0 + y, mask);
		}
		hd_fog_col.resize(c.w);
		hd_fog_u.resize(c.w);
		for (int x = 0; x != c.w; ++x) {
			int cell;
			grid(x, hd_cam_x, cell, hd_fog_u[x]);
			hd_fog_col[x] = cell - gx0;
		}
		// Brightness 0..255 for a level scaled by 256 * 256 (looked up at 1/256 precision).
		if (hd_fog_scale.empty()) {
			hd_fog_scale.resize(31 * 256 + 1);
			for (int i = 0; i <= 31 * 256; ++i) hd_fog_scale[i] = (uint8_t)((i * 255 + 31 * 128) / (31 * 256));
		}
		scr::parallel_ranges((size_t)c.h, 16, [&](size_t from, size_t to) {
			std::vector<int> col(gw);  // the row's level at each grid column, scaled by 256
			for (int y = (int)from; y != (int)to; ++y) {
				int cy, v;
				grid(y, hd_cam_y, cy, v);
				const uint8_t* top = &hd_fog_levels[(size_t)(cy - gy0) * gw];
				const uint8_t* bottom = top + gw;
				bool all_clear = true;
				for (int i = 0; i != gw; ++i) {
					col[i] = top[i] * (256 - v) + bottom[i] * v;
					all_clear &= col[i] == 31 * 256;
				}
				if (all_clear) continue;
				uint32_t* row = c.px + (size_t)y * c.pitch;
				for (int x = 0; x != c.w;) {
					int i = hd_fog_col[x];
					int end = x;
					while (end != c.w && hd_fog_col[end] == i) ++end;
					int l = col[i], r = col[i + 1];
					if (l == 31 * 256 && r == 31 * 256) {
						x = end;
						continue;
					}
					for (; x != end; ++x) {
						int u = hd_fog_u[x];
						uint32_t k = hd_fog_scale[(l * (256 - u) + r * u) >> 8];
						uint32_t p = row[x];
						uint32_t rb = (p & 0x00ff00ff) * k + 0x00800080;
						rb = ((rb + ((rb >> 8) & 0x00ff00ff)) >> 8) & 0x00ff00ff;
						uint32_t g = ((p >> 8) & 0xff) * k + 0x80;
						g = ((g + (g >> 8)) >> 8) & 0xff;
						row[x] = rb | g << 8 | 0xff000000u;
					}
				}
			}
		});
	}
	std::vector<uint8_t> hd_fog_levels, hd_fog_scale;
	std::vector<int> hd_fog_col, hd_fog_u;

	// A palette index the overlay pass never writes (health bars, the minimap view box,
	// selection colours), used as "transparent" in the indexed buffer.
	uint8_t hd_overlay_sentinel() const {
		std::array<bool, 256> used{};
		for (auto c : img.hp_bar_colors) used[(uint8_t)c] = true;
		used[255] = true;
		used[0] = true;
		used[(uint8_t)tileset_img.resource_minimap_color] = true;
		for (auto& colors : img.player_unit_colors) for (auto c : colors) used[(uint8_t)c] = true;
		for (int i = 1; i != 255; ++i) if (!used[i]) return (uint8_t)i;
		return 1;
	}

	// Lays the indexed overlay (logical pixels) over the canvas, nearest-neighbour scaled.
	// Inside `opaque` (the minimap) every pixel is copied. The overlay is mostly sentinel,
	// which is skipped 8 bytes at a time; rows in parallel.
	void hd_composite_overlay(const uint8_t* data, size_t pitch, uint8_t sentinel, rect opaque) {
		auto& c = hd_canvas;
		std::array<uint32_t, 256> rgb;
		for (size_t i = 0; i != 256; ++i) rgb[i] = hd_palette_rgb(i);
		// Window column span of each logical column: window x samples logical column
		// min(W - 1, x / zoom); a column no window x samples gets an empty span.
		const int lw = (int)screen_width;
		hd_overlay_x.assign(lw + 1, c.w);
		for (int x = c.w - 1; x >= 0; --x) hd_overlay_x[std::min(lw - 1, (int)(x / zoom))] = x;
		for (int l = lw - 1; l >= 0; --l) hd_overlay_x[l] = std::min(hd_overlay_x[l], hd_overlay_x[l + 1]);
		const uint64_t pattern = sentinel * 0x0101010101010101ull;
		const int ox0 = std::max(0, opaque.from.x), ox1 = std::min(lw, opaque.to.x);
		scr::parallel_ranges((size_t)c.h, 16, [&](size_t from, size_t to) {
			for (int y = (int)from; y != (int)to; ++y) {
				int ly = (int)(y / zoom);
				if (ly >= (int)screen_height) break;
				const uint8_t* src = data + (size_t)ly * pitch;
				uint32_t* dst = c.px + (size_t)y * c.pitch;
				auto put = [&](int l) {
					std::fill(dst + hd_overlay_x[l], dst + hd_overlay_x[l + 1], rgb[src[l]]);
				};
				for (int l = 0; l < lw;) {
					if (l + 8 <= lw) {
						uint64_t w;
						std::memcpy(&w, src + l, 8);
						if (w == pattern) {
							l += 8;
							continue;
						}
					}
					if (src[l] != sentinel) put(l);
					++l;
				}
				if (ly >= opaque.from.y && ly < opaque.to.y) {
					for (int l = ox0; l < ox1; ++l) put(l);
				}
			}
		});
	}
	std::vector<int> hd_overlay_x;

	void hd_draw_rect_outline(rect r, uint32_t colour) {
		auto& c = hd_canvas;
		int x0 = std::max(0, (int)(r.from.x * zoom)), x1 = std::min(c.w - 1, (int)(r.to.x * zoom));
		int y0 = std::max(0, (int)(r.from.y * zoom)), y1 = std::min(c.h - 1, (int)(r.to.y * zoom));
		if (x0 > x1 || y0 > y1) return;
		for (int x = x0; x <= x1; ++x) c.px[(size_t)y0 * c.pitch + x] = c.px[(size_t)y1 * c.pitch + x] = colour;
		for (int y = y0; y <= y1; ++y) c.px[(size_t)y * c.pitch + x0] = c.px[(size_t)y * c.pitch + x1] = colour;
	}
#else
	bool hd_ready() { return false; }
	void hd_prepare_sprites() {}
	void hd_draw_image(const image_t*, size_t) {}
	void hd_draw_selection_circle(const sprite_t*, const unit_t*) {}
#endif

	fp8 game_speed = fp8::integer(1);

	std::unique_ptr<native_window_drawing::surface> window_surface;
	std::unique_ptr<native_window_drawing::surface> indexed_surface;
	std::unique_ptr<native_window_drawing::surface> rgba_surface;
	native_window_drawing::palette* palette = nullptr;
	std::chrono::high_resolution_clock clock;
	std::chrono::high_resolution_clock::time_point last_draw;
	std::chrono::high_resolution_clock::time_point last_input_poll;
	std::chrono::high_resolution_clock::time_point last_fps;
	int fps_counter = 0;
	int refresh_hz = 0;  // of the window's display; 0 if unknown

	// Frame pacing for the main loop: a frame per display refresh (60 Hz if unknown), so
	// scrolling moves every time the screen does. With vsync presentation the present call
	// itself waits for the refresh; half an interval is then only a floor, for when it
	// returns at once (a hidden window, a driver that ignores vsync).
	std::chrono::microseconds frame_interval() const {
		int hz = refresh_hz > 0 ? std::max(30, std::min(240, refresh_hz)) : 60;
		auto interval = std::chrono::microseconds(1000000 / hz);
		return wnd && wnd.has_vsync() ? interval / 2 : interval;
	}
	size_t scroll_speed_n = 0;
	std::chrono::high_resolution_clock::duration scroll_held{};  // how long the current scroll has lasted
	double scroll_rem_x = 0, scroll_rem_y = 0;  // sub-pixel camera movement not yet applied

	// Zoom: the game is drawn into a logical screen_width x screen_height buffer
	// (window size / zoom) and scaled to the window, so every draw/clip path
	// stays in logical pixels; mouse input is mapped back by dividing by zoom.
	double zoom = 1.0;
	int window_width = 0;
	int window_height = 0;
	// The game view is capped at max_aspect (16:9) and centred; wider windows get black
	// pillarbox bars. viewport_x/viewport_w are the view's position/width in window pixels.
	double max_aspect = 16.0 / 9.0;
	int viewport_x = 0;
	int viewport_w = 0;
	bool edge_pan = true;
	int edge_pan_px = 4;
	// Keyboard and edge scroll speed, a level in the settings menu: level 3 is the original
	// speed, and the default (5) scrolls twice as fast.
	enum { scroll_speed_levels = 9 };
	int scroll_speed_level = 5;
	double scroll_speed_factor() const {
		static const double factors[scroll_speed_levels] = {0.5, 0.75, 1, 1.5, 2, 2.5, 3, 4, 5};
		return factors[std::max(1, std::min((int)scroll_speed_levels, scroll_speed_level)) - 1];
	}

	void resize(int width, int height) {
		if (!wnd && create_window) wnd.create("OpenBW", 0, 0, width, height);
		window_width = width;
		window_height = height;
		viewport_w = std::min(width, (int)(height * max_aspect + 0.5));
		viewport_x = (width - viewport_w) / 2;
		screen_width = std::max(64, (int)(viewport_w / zoom));
		screen_height = std::max(64, (int)(height / zoom));
		//view_scale = fp16::integer(1) - (fp16::integer(1) / 4);
		view_scale = fp16::integer(1);
		view_width = (fp16::integer(screen_width) / view_scale).integer_part();
		view_height = (fp16::integer(screen_height) / view_scale).integer_part();
		view_scale = (ufp16::integer(screen_width) / view_width).as_signed();
		window_surface.reset();
		indexed_surface.reset();
		rgba_surface.reset();
		// With pillarbox bars, keep the cursor in the game view (edge pan then triggers at
		// the bars); without bars leave it free to leave the window.
		if (wnd) {
			if (viewport_x > 0) wnd.confine_cursor(viewport_x, 0, viewport_w, height);
			else wnd.confine_cursor(0, 0, 0, 0);
		}
	}

	// Keeps the map point under window pixel (anchor_x, anchor_y) fixed;
	// the default anchor (-1) is the window centre.
	void set_zoom(double new_zoom, int anchor_x = -1, int anchor_y = -1) {
		if (new_zoom == zoom || window_width <= 0) return;
		if (anchor_x < 0 || anchor_y < 0) {
			anchor_x = viewport_w / 2;
			anchor_y = window_height / 2;
		}
		xy anchor = screen_pos + xy((int)(anchor_x / zoom), (int)(anchor_y / zoom));
		zoom = new_zoom;
		resize(window_width, window_height);
		screen_pos = anchor - xy((int)(anchor_x / zoom), (int)(anchor_y / zoom));
	}

	static const std::array<double, 9>& zoom_steps() {
		static const std::array<double, 9> steps = {{0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0}};
		return steps;
	}

	static double nearest_zoom_step(double z) {
		double best = 1.0;
		for (double s : zoom_steps()) if (std::abs(s - z) < std::abs(best - z)) best = s;
		return best;
	}

	void step_zoom(int direction, int anchor_x = -1, int anchor_y = -1) {
		auto& zoom_steps = this->zoom_steps();
		const int n = (int)zoom_steps.size();
		int i = 0;
		while (i + 1 < n && zoom_steps[i] < zoom - 1e-9) ++i;
		i += direction;
		if (i < 0) i = 0;
		if (i >= n) i = n - 1;
		set_zoom(zoom_steps[i], anchor_x, anchor_y);
	}

	// Doubling steps from 1/4x to 128x, with 1.5x between 1x and 2x.
	void step_speed(bool faster) {
		const fp8 one = fp8::integer(1), one_half = fp8::from_raw(one.raw_value * 3 / 2), two = fp8::integer(2);
		if (faster) {
			if (game_speed == one) game_speed = one_half;
			else if (game_speed == one_half) game_speed = two;
			else if (game_speed < fp8::integer(128)) game_speed *= 2;
		} else {
			if (game_speed == two) game_speed = one_half;
			else if (game_speed == one_half) game_speed = one;
			else if (game_speed > fp8::from_raw(one.raw_value / 4)) game_speed /= 2;  // slowest 1/4x
		}
	}

	int unmuted_volume = 50;

	// ---- Stats overlay (minerals, gas, supply, army/worker supply per player) ----
	// Drawn into its own surface and blitted onto the window after zoom scaling, so it
	// stays crisp and constant-size. Supply is shown as Brood War displays it (internal
	// half-units rounded up). Workers = worker units alive; army = supply used - workers
	// (so units in production, including workers being trained, count as army).
	bool show_hud = false;
	std::unique_ptr<native_window_drawing::surface> hud_surface;

	static uint32_t hud_rgba(int r, int g, int b, int a) {
		return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
	}

	void hud_text(uint32_t* buf, size_t pitch, int w, int h, int x, int y, const char* text, int r, int g, int b) {
		for (; *text; ++text, x += hud_font::cell_w) {
			int c = (unsigned char)*text;
			if (c < 32 || c > 126) c = '?';
			const uint8_t* glyph = hud_font::glyphs[c - 32];
			for (int gy = 0; gy != hud_font::cell_h; ++gy) {
				if (y + gy < 0 || y + gy >= h) continue;
				for (int gx = 0; gx != hud_font::cell_w; ++gx) {
					if (x + gx < 0 || x + gx >= w) continue;
					int cov = glyph[gy * hud_font::cell_w + gx];
					if (!cov) continue;
					uint32_t& px = buf[(size_t)(y + gy) * pitch + (size_t)(x + gx)];
					int pa = px >> 24, pb = (px >> 16) & 0xff, pg = (px >> 8) & 0xff, pr = px & 0xff;
					px = hud_rgba(pr + (r - pr) * cov / 255, pg + (g - pg) * cov / 255, pb + (b - pb) * cov / 255, pa + (255 - pa) * cov / 255);
				}
			}
		}
	}

	struct hud_row {
		a_string name;
		int player = 0;
		int race = 0;
		int minerals = 0, gas = 0, supply = 0, supply_max = 0, army = 0, workers = 0;
		int r = 255, g = 255, b = 255;
	};

	a_vector<hud_row> hud_rows() {
		a_vector<hud_row> rows;
		for (int i = 0; i != 8; ++i) {
			auto& pl = st.players[i];
			if (!pl.initially_active) continue;
			int race = (int)pl.race;
			if (race < 0 || race > 2) continue;
			hud_row row;
			row.name = replay_st.player_name[i].empty() ? a_string("Player ") + std::to_string(i + 1).c_str() : replay_st.player_name[i];
			row.race = race;
			row.player = i;
			row.minerals = st.current_minerals[i];
			row.gas = st.current_gas[i];
			int used = (int)st.supply_used[i][race].raw_value;
			int avail = std::min((int)st.supply_available[i][race].raw_value, 400);
			row.supply = (used + 1) / 2;
			row.supply_max = avail / 2;
			for (unit_t* u : ptr(st.player_units[i])) {
				if (ut_worker(u->unit_type)) ++row.workers;
			}
			row.army = std::max(0, row.supply - row.workers);
			if (palette && tileset_img.wpe.size() == 256 * 4) {
				size_t ci = img.player_minimap_colors.at(player_color(i));
				row.r = tileset_img.wpe[4 * ci + 0];
				row.g = tileset_img.wpe[4 * ci + 1];
				row.b = tileset_img.wpe[4 * ci + 2];
			}
			rows.push_back(row);
		}
		return rows;
	}

	void draw_hud() {
		auto rows = hud_rows();
		if (rows.empty()) return;
		static const char* race_letter = "ZTP";
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 6;
		const int name_cols = 18;
		// columns: swatch+name | Min | Gas | Supply | Army | Wkr
		const int cols = 2 + name_cols + 7 + 7 + 10 + 7 + 6;
		int w = cols * cw + pad * 2;
		int h = (int)(rows.size() + 1) * ch + pad * 2;
		if (!hud_surface || hud_surface->w != w || hud_surface->h != h) {
			hud_surface = native_window_drawing::create_rgba_surface(w, h);
			hud_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)hud_surface->lock();
		size_t pitch = hud_surface->pitch / 4;
		for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) buf[(size_t)y * pitch + x] = hud_rgba(8, 10, 16, 175);
		char line[128];
		std::snprintf(line, sizeof(line), "%-*s %6s %6s %9s %6s %5s", name_cols + 1, "", "Min", "Gas", "Supply", "Army", "Wkr");
		hud_text(buf, pitch, w, h, pad, pad, line, 170, 180, 200);
		for (size_t i = 0; i != rows.size(); ++i) {
			auto& row = rows[i];
			int y = pad + (int)(i + 1) * ch;
			for (int sy = 5; sy != ch - 5; ++sy) for (int sx = 1; sx != cw - 1; ++sx) buf[(size_t)(y + sy) * pitch + pad + sx] = hud_rgba(row.r, row.g, row.b, 255);
			char name[64];
			std::snprintf(name, sizeof(name), "%.*s (%c)", name_cols - 4, row.name.c_str(), race_letter[row.race]);
			hud_text(buf, pitch, w, h, pad + 2 * cw, y, name, row.r, row.g, row.b);
			char sup[16];
			std::snprintf(sup, sizeof(sup), "%d/%d", row.supply, row.supply_max);
			std::snprintf(line, sizeof(line), "%6d %6d %9s %6d %5d", row.minerals, row.gas, sup, row.army, row.workers);
			hud_text(buf, pitch, w, h, pad + (name_cols + 2) * cw, y, line, 235, 235, 235);
		}
		hud_surface->unlock();
	}

	// ---- Selection panel (unit stats + wireframes, like the in-game console) ----
	// One selected unit: its large wireframe (unit/wirefram/wirefram.grp) beside its stats.
	// Several: a grid of small wireframes (grpwire.grp). Drawn crisp after zoom scaling,
	// bottom-centre of the game view. Wireframe pixels are palette *roles*, not colours;
	// see wire_pixel_rgb for how they are remapped by health and shields. Which hull
	// section turns first is our approximation (the exact per-unit pattern is undocumented):
	// sections step green -> yellow -> red in an order fixed by the unit's
	// wireframe_randomizer.
	bool show_selection_panel = true;
	bool selection_panel_data_loaded = false;
	grp_t wireframe_grp, group_wireframe_grp;
	a_vector<bool> group_wireframe_valid;  // grpwire.grp frames for buildings etc. are "BLANK" art
	grp_t cmdicons_grp;
	std::array<uint8_t, 24> twire_remap{};
	bool twire_loaded = false;
	std::array<uint8_t, 16> ticon_remap{};
	string_table_data stat_txt;
	std::unique_ptr<native_window_drawing::surface> panel_surface;

	void load_selection_panel_data() {
		try {
			a_vector<uint8_t> data;
			load_data_file(data, "unit/wirefram/wirefram.grp");
			wireframe_grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
			load_data_file(data, "unit/wirefram/grpwire.grp");
			group_wireframe_grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
			group_wireframe_valid.clear();
			for (auto& f : group_wireframe_grp.frames) group_wireframe_valid.push_back(frame_has_wireframe_roles(f));
			load_data_file(stat_txt.data, "rez/stat_txt.tbl");
			load_data_file(data, "game/twire.pcx");
			auto twire = load_pcx_data(data);
			if (twire.data.size() >= 24) {
				std::copy(twire.data.begin(), twire.data.begin() + 24, twire_remap.begin());
				twire_loaded = true;
			}
			load_data_file(data, "unit/cmdbtns/cmdicons.grp");
			cmdicons_grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
			load_data_file(data, "unit/cmdbtns/ticon.pcx");
			auto ticon = load_pcx_data(data);
			if (ticon.data.size() < 16) error("ticon.pcx too small");
			std::copy(ticon.data.begin(), ticon.data.begin() + 16, ticon_remap.begin());
			selection_panel_data_loaded = true;
		} catch (const std::exception& e) {
			ui::log("selection panel disabled: %s\n", e.what());
		}
	}

	// stat_txt lookup with bounds checks; strips the hotkey/format control bytes.
	a_string stat_text(int index) const {
		const auto& d = stat_txt.data;
		if (index <= 0 || d.size() < 2) return {};
		size_t count = d[0] | (size_t)d[1] << 8;
		if ((size_t)index > count || 2 + (size_t)index * 2 > d.size()) return {};
		size_t offset = d[2 + (index - 1) * 2] | (size_t)d[3 + (index - 1) * 2] << 8;
		a_string s;
		for (size_t i = offset; i < d.size() && d[i]; ++i) {
			if (d[i] >= 32 && d[i] < 127) s += (char)d[i];
		}
		return s;
	}

	struct wire_rgb { int r, g, b; };

	// Colour for one wireframe pixel. section_state: 0 green, 1 yellow, 2 red.
	static wire_rgb wire_section_rgb(int state) {
		static const wire_rgb c[3] = {{24, 210, 24}, {236, 228, 40}, {222, 32, 24}};
		return c[std::max(0, std::min(2, state))];
	}

	// Section i (0..3) state for a unit at hp fraction f: 8 damage steps are spread over
	// the sections in a per-unit order, first turning them yellow, then red.
	static int wire_section_state(int section, double hp_frac, int randomizer) {
		static const uint8_t perms[24][4] = {
			{0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},{1,0,2,3},{1,0,3,2},
			{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},{2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},
			{2,3,0,1},{2,3,1,0},{3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}};
		int steps = (int)((1.0 - std::max(0.0, std::min(1.0, hp_frac))) * 8.0 + 0.5);
		const uint8_t* p = perms[(unsigned)randomizer % 24];
		int rank = 0;
		while (rank != 4 && p[rank] != section) ++rank;
		return (steps > rank) + (steps > rank + 4);
	}

	static int overall_state(double hp_frac) {
		return hp_frac > 0.66 ? 0 : hp_frac > 0.33 ? 1 : 2;
	}

	struct unit_vitals {
		double hp = 1, shields = 0;
		bool has_shields = false;
		int randomizer = 0;
	};

	unit_vitals vitals_of(const unit_t* u) const {
		unit_vitals v;
		double max_hp = u->unit_type->hitpoints.raw_value;
		v.hp = max_hp > 0 ? u->hp.raw_value / max_hp : 1.0;
		v.has_shields = u->unit_type->has_shield && u->unit_type->shield_points > 0;
		if (v.has_shields) v.shields = (double)u->shield_points.raw_value / (u->unit_type->shield_points * 256.0);
		v.randomizer = u->wireframe_randomizer;
		return v;
	}

	// Brood War remaps each wireframe role through game/twire.pcx (24 palette indices) by
	// missing hp/shields; stages run from full health. Tables as documented by
	// DarkenedFantasies (staredit.net topic 18436): 192/193 shield outline (inner/outer),
	// 208-211 hull sections (randomised pattern per unit), 216-219 the Zerg "heatmap"
	// (bright/dark inner, bright/dark outer). Unshielded units' outline maps to the
	// shields-gone entry, which is near-black.
	static int wire_stage(double missing_frac, int stages) {
		return std::max(0, std::min(stages - 1, (int)(std::max(0.0, missing_frac) * stages)));
	}

	wire_rgb twire_rgb(int position) const {
		if (!twire_loaded || tileset_img.wpe.size() != 256 * 4) return {120, 120, 120};
		size_t ci = twire_remap[std::max(0, std::min(23, position))];
		return {tileset_img.wpe[4 * ci], tileset_img.wpe[4 * ci + 1], tileset_img.wpe[4 * ci + 2]};
	}

	wire_rgb wire_pixel_rgb(int index, const unit_vitals& v) const {
		static const int inner_shield[8] = {3, 4, 5, 6, 7, 8, 9, 2};
		static const int outer_shield[8] = {4, 5, 6, 7, 8, 9, 2, 2};
		static const int section[3] = {1, 0, 10};
		static const int heat[4][6] = {
			{0, 1, 1, 19, 19, 14},     // 216 bright inner
			{1, 18, 18, 11, 11, 6},    // 217 dark inner
			{18, 20, 20, 10, 13, 7},   // 218 bright outer
			{10, 12, 16, 16, 6, 8}};   // 219 dark outer
		if (!twire_loaded) {
			auto c = wire_section_rgb(overall_state(v.hp));
			return index == 192 || index == 193 ? wire_rgb{56, 120, 252} : c;
		}
		double shields_missing = v.has_shields ? 1.0 - v.shields : 1.0;
		if (index == 192) return twire_rgb(inner_shield[wire_stage(shields_missing, 8)]);
		if (index == 193) return twire_rgb(outer_shield[wire_stage(shields_missing, 8)]);
		if (index >= 208 && index <= 211) return twire_rgb(section[wire_section_state(index - 208, v.hp, v.randomizer)]);
		if (index >= 216 && index <= 219) return twire_rgb(heat[index - 216][wire_stage(1.0 - v.hp, 6)]);
		return {120, 120, 120};
	}

	// Draws grp frame `frame_index` at (x, y), nearest-neighbour scaled by num/den.
	void draw_wireframe(uint32_t* buf, size_t pitch, int w, int h, int x, int y, const grp_t& grp, size_t frame_index, int num, int den, const unit_vitals& v) {
		if (frame_index >= grp.frames.size()) return;
		auto& f = grp.frames[frame_index];
		for (size_t fy = 0; fy != f.size.y; ++fy) {
			const uint8_t* p = f.data_container.data() + f.line_data_offset[fy];
			size_t fx = 0;
			auto put = [&](int index) {
				auto c = wire_pixel_rgb(index, v);
				uint32_t px = hud_rgba(c.r, c.g, c.b, 255);
				int sx = (int)(f.offset.x + fx), sy = (int)(f.offset.y + fy);
				for (int Y = y + sy * num / den; Y < y + (sy + 1) * num / den; ++Y) {
					for (int X = x + sx * num / den; X < x + (sx + 1) * num / den; ++X) {
						if (X >= 0 && X < w && Y >= 0 && Y < h) buf[(size_t)Y * pitch + X] = px;
					}
				}
			};
			while (fx < f.size.x) {
				uint8_t c = *p++;
				if (c & 0x80) {
					fx += c & 0x7f;
				} else if (c & 0x40) {
					uint8_t index = *p++;
					for (int i = 0; i != (c & 0x3f); ++i, ++fx) put(index);
				} else {
					for (int i = 0; i != c; ++i, ++fx) put(*p++);
				}
			}
		}
	}

#ifdef OPENBW_REMASTERED
	// The selection panel's wireframe of `ut` in the remastered art, `size` pixels square at
	// (x, y) of an RGBA buffer, coloured by health and shields like the classic one (see
	// wire_pixel_rgb). False, drawing nothing, while the classic graphics are in use or the
	// install lacks it.
	bool hd_draw_wireframe(uint32_t* buf, size_t pitch, int w, int h, int x, int y, const unit_type_t* ut, int size, const unit_vitals& v) {
		if (!use_remastered || !hd) return false;
		auto* layers = hd->wireframe((size_t)ut->id, size);
		if (!layers) return false;
		auto rgb = [](wire_rgb c) { return scr::rgba(c.r, c.g, c.b, 255); };
		bool zerg = ut->group_flags & GroupFlags::Zerg;
		int first_section = zerg ? 216 : 208;
		std::array<uint32_t, 4> sections;
		for (int k = 0; k != 4; ++k) sections[k] = rgb(wire_pixel_rgb(first_section + k, v));
		scr::image img = scr::composite_wireframe(*layers, sections, rgb(wire_pixel_rgb(193, v)), rgb(wire_pixel_rgb(192, v)), zerg);
		for (int sy = std::max(0, -y); sy < img.h && y + sy < h; ++sy) {
			uint32_t* dst = buf + (size_t)(y + sy) * pitch + x;
			const uint32_t* src = &img.px[(size_t)sy * img.w];
			for (int sx = std::max(0, -x); sx < img.w && x + sx < w; ++sx) {
				uint32_t s = src[sx], a = scr::ch_a(s);
				if (!a) continue;
				uint32_t d = dst[sx], ia = 255 - a;
				dst[sx] = scr::rgba(scr::ch_r(s) + scr::mul255(scr::ch_r(d), ia), scr::ch_g(s) + scr::mul255(scr::ch_g(d), ia),
				                    scr::ch_b(s) + scr::mul255(scr::ch_b(d), ia), 255);
			}
		}
		return true;
	}
#else
	bool hd_draw_wireframe(uint32_t*, size_t, int, int, int, int, const unit_type_t*, int, const unit_vitals&) { return false; }
#endif

	static bool is_wireframe_role(int index) {
		return index == 192 || index == 193 || (index >= 208 && index <= 211) || (index >= 216 && index <= 219);
	}

	// The placeholder frames (grpwire.grp's "BLANK" text for most buildings) are drawn in a
	// single colour; real wireframes use several roles.
	static bool frame_has_wireframe_roles(const grp_t::frame_t& f) {
		int first = -1;
		auto check = [&](int index) {
			if (!is_wireframe_role(index)) return false;
			if (first == -1) first = index;
			return index != first;
		};
		for (size_t fy = 0; fy != f.size.y; ++fy) {
			const uint8_t* p = f.data_container.data() + f.line_data_offset[fy];
			for (size_t fx = 0; fx < f.size.x;) {
				uint8_t c = *p++;
				if (c & 0x80) {
					fx += c & 0x7f;
				} else if (c & 0x40) {
					if (check(*p++)) return true;
					fx += c & 0x3f;
				} else {
					for (int i = 0; i != c; ++i, ++fx) if (check(*p++)) return true;
				}
			}
		}
		return false;
	}

	// What the console shows a unit as: a Hatchery morphing into a Lair is still a Hatchery
	// to the engine until the morph completes, but Brood War presents it as the Lair (as
	// with Hive, Greater Spire and Sunken/Spore Colonies).
	const unit_type_t* shown_type(const unit_t* u) const {
		return unit_is_morphing_building(u) ? u->build_queue.front() : u->unit_type;
	}

	a_vector<unit_t*> selected_units() {
		a_vector<unit_t*> r;
		for (auto uid : current_selection) {
			if (unit_t* u = get_unit(uid)) r.push_back(u);
		}
		return r;
	}

	a_string player_label(int owner) const {
		if (owner < 0 || owner >= 12) return "?";
		if (owner == 11) return "Neutral";
		if (owner < 8 && !replay_st.player_name[owner].empty()) return replay_st.player_name[owner];
		return a_string("Player ") + std::to_string(owner + 1).c_str();
	}

	wire_rgb player_rgb(int owner) const {
		if (owner < 0 || owner >= 8 || !palette || tileset_img.wpe.size() != 256 * 4) return {220, 220, 220};
		size_t ci = img.player_minimap_colors.at(player_color(owner));
		return {tileset_img.wpe[4 * ci + 0], tileset_img.wpe[4 * ci + 1], tileset_img.wpe[4 * ci + 2]};
	}

	// ---- Single-unit console, laid out like Brood War's own status panel ----
	// Under the wireframe: shields, hp, energy. Beside it: name, owner, kills, resource or
	// supply text, then status icons from unit/cmdbtns/cmdicons.grp: armor, shields and
	// weapons carry their upgrade level, interceptors/scarabs/mines their count. Below
	// those, any training queue, research, upgrade, add-on or construction progress.
	struct panel_line {
		a_string text;
		wire_rgb rgb{230, 230, 230};
	};
	struct console_icon {
		int frame;          // cmdicons.grp frame
		int number = -1;    // drawn bottom-right when >= 0
		a_string name = {};  // shown when hovered, if set
	};
	struct console_progress {
		a_vector<int> frames;  // first is the item in progress; the rest are queued
		double frac = 0;
		a_string label;        // used when there is no icon (construction)
	};
	struct console_info {
		a_vector<panel_line> vitals, lines;
		a_vector<console_icon> icons;
		a_vector<console_icon> research;  // completed research that affects this unit
		a_vector<console_progress> progress;
		a_vector<const unit_t*> cargo;
	};

	static double progress_frac(int done, int total) {
		if (total <= 0) return 0;
		return std::max(0.0, std::min(1.0, (double)done / total));
	}

	// Research (single-level upgrades and techs) that changes what a unit type does, in the
	// order the game's buttons list them. Techs every player starts with are left out.
	struct research_item {
		bool tech;
		int id;  // TechTypes or UpgradeTypes
	};
	static a_vector<research_item> research_affecting(UnitTypes t) {
		using U = UpgradeTypes;
		using T = TechTypes;
		auto up = [](U u) { return research_item{false, (int)u}; };
		auto te = [](T x) { return research_item{true, (int)x}; };
		switch (t) {
		case UnitTypes::Terran_Marine: return {te(T::Stim_Packs), up(U::U_238_Shells)};
		case UnitTypes::Terran_Firebat: return {te(T::Stim_Packs)};
		case UnitTypes::Terran_Medic: return {te(T::Restoration), te(T::Optical_Flare), up(U::Caduceus_Reactor)};
		case UnitTypes::Terran_Ghost: return {te(T::Lockdown), te(T::Personnel_Cloaking), up(U::Ocular_Implants), up(U::Moebius_Reactor)};
		case UnitTypes::Terran_Vulture: return {up(U::Ion_Thrusters)};
		case UnitTypes::Terran_Siege_Tank_Tank_Mode:
		case UnitTypes::Terran_Siege_Tank_Siege_Mode: return {te(T::Tank_Siege_Mode)};
		case UnitTypes::Terran_Goliath: return {up(U::Charon_Boosters)};
		case UnitTypes::Terran_Wraith: return {te(T::Cloaking_Field), up(U::Apollo_Reactor)};
		case UnitTypes::Terran_Science_Vessel: return {te(T::EMP_Shockwave), te(T::Irradiate), up(U::Titan_Reactor)};
		case UnitTypes::Terran_Battlecruiser: return {te(T::Yamato_Gun), up(U::Colossus_Reactor)};
		case UnitTypes::Zerg_Drone: return {te(T::Burrowing)};
		case UnitTypes::Zerg_Zergling: return {up(U::Metabolic_Boost), up(U::Adrenal_Glands), te(T::Burrowing)};
		case UnitTypes::Zerg_Hydralisk: return {up(U::Muscular_Augments), up(U::Grooved_Spines), te(T::Lurker_Aspect), te(T::Burrowing)};
		case UnitTypes::Zerg_Overlord: return {up(U::Ventral_Sacs), up(U::Antennae), up(U::Pneumatized_Carapace)};
		case UnitTypes::Zerg_Ultralisk: return {up(U::Anabolic_Synthesis), up(U::Chitinous_Plating)};
		case UnitTypes::Zerg_Queen: return {te(T::Spawn_Broodlings), te(T::Ensnare), up(U::Gamete_Meiosis)};
		case UnitTypes::Zerg_Defiler: return {te(T::Plague), te(T::Consume), up(U::Metasynaptic_Node), te(T::Burrowing)};
		case UnitTypes::Zerg_Infested_Terran: return {te(T::Burrowing)};
		case UnitTypes::Protoss_Zealot: return {up(U::Leg_Enhancements)};
		case UnitTypes::Protoss_Dragoon: return {up(U::Singularity_Charge)};
		case UnitTypes::Protoss_High_Templar: return {te(T::Psionic_Storm), te(T::Hallucination), up(U::Khaydarin_Amulet)};
		case UnitTypes::Protoss_Dark_Archon: return {te(T::Mind_Control), te(T::Maelstrom), up(U::Argus_Talisman)};
		case UnitTypes::Protoss_Reaver: return {up(U::Scarab_Damage), up(U::Reaver_Capacity)};
		case UnitTypes::Protoss_Shuttle: return {up(U::Gravitic_Drive)};
		case UnitTypes::Protoss_Observer: return {up(U::Sensor_Array), up(U::Gravitic_Boosters)};
		case UnitTypes::Protoss_Scout: return {up(U::Apial_Sensors), up(U::Gravitic_Thrusters)};
		case UnitTypes::Protoss_Carrier: return {up(U::Carrier_Capacity)};
		case UnitTypes::Protoss_Arbiter: return {te(T::Recall), te(T::Stasis_Field), up(U::Khaydarin_Core)};
		case UnitTypes::Protoss_Corsair: return {te(T::Disruption_Web), up(U::Argus_Jewel)};
		default: return {};
		}
	}

	console_info console_info_of(const unit_t* u) const {
		console_info ci;
		char buf[96];
		const unit_type_t* ut = u->unit_type;
		auto v = vitals_of(u);
		if (v.has_shields) {
			std::snprintf(buf, sizeof(buf), "%d/%d", (int)u->shield_points.integer_part(), ut->shield_points);
			ci.vitals.push_back({buf, {225, 225, 225}});
		}
		if (!ut_resource(u) && !(ut->flags & unit_type_t::flag_invincible)) {
			std::snprintf(buf, sizeof(buf), "%d/%d", (int)u->hp.ceil().integer_part(), (int)ut->hitpoints.integer_part());
			ci.vitals.push_back({buf, wire_section_rgb(overall_state(v.hp))});
		}
		if (ut_has_energy(u) && u_completed(u)) {
			std::snprintf(buf, sizeof(buf), "%d/%d", (int)u->energy.integer_part(), (int)unit_max_energy(u).integer_part());
			ci.vitals.push_back({buf, {225, 225, 225}});
		}

		ci.lines.push_back({stat_text((int)shown_type(u)->id + 1), {255, 255, 255}});
		if (u->owner != 11) ci.lines.push_back({player_label(u->owner), player_rgb(u->owner)});
		const unit_t* armed = u->subunit ? u->subunit : u;
		bool has_weapon = unit_ground_weapon(armed) || unit_air_weapon(armed) || unit_is_carrier(u) || unit_is_reaver(u);
		if (has_weapon && u_completed(u)) {
			std::snprintf(buf, sizeof(buf), "Kills: %d", u->kill_count);
			ci.lines.push_back({buf});
		}
		if (ut_resource(u)) {
			bool minerals = unit_is_mineral_field(u);
			std::snprintf(buf, sizeof(buf), "%s: %d", minerals ? "Minerals" : "Vespene Gas", u->building.resource.resource_count);
			ci.lines.push_back({buf});
		}
		int race = (int)st.players[u->owner < 8 ? u->owner : 0].race;
		if (u->owner < 8 && race >= 0 && race <= 2 && u_completed(u) && ut->supply_provided.raw_value > 0 && ut_building(u)) {
			static const char* used_fmt[3] = {"Control Used: %d", "Supplies Used: %d", "Psi Used: %d"};
			static const char* prov_fmt[3] = {"Control Provided: %d", "Supplies Provided: %d", "Psi Provided: %d"};
			static const char* total_fmt[3] = {"Total Control: %d", "Total Supplies: %d", "Total Psi: %d"};
			static const char* max_fmt[3] = {"Control Max: %d", "Supplies Max: %d", "Psi Max: %d"};
			std::snprintf(buf, sizeof(buf), used_fmt[race], ((int)st.supply_used[u->owner][race].raw_value + 1) / 2);
			ci.lines.push_back({buf});
			std::snprintf(buf, sizeof(buf), prov_fmt[race], (int)ut->supply_provided.raw_value / 2);
			ci.lines.push_back({buf});
			std::snprintf(buf, sizeof(buf), total_fmt[race], std::min((int)st.supply_available[u->owner][race].raw_value, 400) / 2);
			ci.lines.push_back({buf});
			std::snprintf(buf, sizeof(buf), max_fmt[race], 200);
			ci.lines.push_back({buf});
		}

		if (u->owner != 11 && !ut_resource(u) && u_completed(u)) {
			// Buildings point at upgrade 60, an unnamed placeholder whose icon index (0) is the
			// Marine portrait; the game shows no armor icon for them, so skip unnamed upgrades.
			auto level_icon = [&](UpgradeTypes upg, int fallback_icon) {
				int icon = fallback_icon;
				if ((int)upg >= 0 && (int)upg < (int)UpgradeTypes::None && get_upgrade_type(upg)->label) icon = get_upgrade_type(upg)->icon;
				int level = player_upgrade_level(u->owner, upg);
				return console_icon{icon, level > 0 ? level : -1};
			};
			ci.icons.push_back(level_icon(ut->armor_upgrade, -1));
			auto* gw = unit_ground_weapon(armed);
			auto* aw = unit_air_weapon(armed);
			// Brood War's order: armor, shields, weapons, then special counts.
			if (v.has_shields) ci.icons.push_back(level_icon(UpgradeTypes::Protoss_Plasma_Shields, -1));
			if (gw) ci.icons.push_back({gw->icon, player_upgrade_level(u->owner, gw->damage_upgrade) > 0 ? player_upgrade_level(u->owner, gw->damage_upgrade) : -1});
			if (aw && aw != gw) ci.icons.push_back({aw->icon, player_upgrade_level(u->owner, aw->damage_upgrade) > 0 ? player_upgrade_level(u->owner, aw->damage_upgrade) : -1});
			if (unit_is_carrier(u)) ci.icons.push_back({(int)UnitTypes::Protoss_Interceptor, (int)unit_interceptor_count(u)});
			if (unit_is_reaver(u)) ci.icons.push_back({(int)UnitTypes::Protoss_Scarab, (int)unit_scarab_count(u)});
			if (unit_is_vulture(u) && player_has_researched(u->owner, TechTypes::Spider_Mines)) {
				ci.icons.push_back({get_tech_type(TechTypes::Spider_Mines)->icon, (int)unit_spider_mine_count(u)});
			}
			ci.icons.erase(std::remove_if(ci.icons.begin(), ci.icons.end(), [](const console_icon& i) { return i.frame < 0; }), ci.icons.end());
			if (u->owner < 8) {
				for (auto r : research_affecting(shown_type(u)->id)) {
					if (r.tech) {
						auto* t = get_tech_type((TechTypes)r.id);
						if (player_has_researched(u->owner, (TechTypes)r.id)) ci.research.push_back({t->icon, -1, stat_text(t->label)});
					} else {
						auto* t = get_upgrade_type((UpgradeTypes)r.id);
						if (player_upgrade_level(u->owner, (UpgradeTypes)r.id) > 0) ci.research.push_back({t->icon, -1, stat_text(t->label)});
					}
				}
			}
		}

		if (!u_completed(u) && !unit_is_egg(u)) {
			// A morphing building counts down the target type's build time.
			const unit_type_t* target = shown_type(u);
			console_progress p;
			p.label = ut_building(u) && target == ut ? "Under construction" : "Morphing";
			p.frac = progress_frac(target->build_time - u->remaining_build_time, target->build_time);
			ci.progress.push_back(p);
		} else if (!u->build_queue.empty() && u->build_queue.front()) {
			console_progress p;
			for (auto* q : u->build_queue) if (q) p.frames.push_back((int)q->id);
			const unit_type_t* q = u->build_queue.front();
			// Eggs and cocoons count down themselves; producers via the unit being built.
			const unit_t* timer = unit_is_egg(u) ? u : u->current_build_unit;
			p.frac = timer ? progress_frac(q->build_time - timer->remaining_build_time, q->build_time) : 0;
			ci.progress.push_back(p);
		}
		if (ut_building(u) && u->building.researching_type) {
			auto* t = u->building.researching_type;
			console_progress p;
			p.frames.push_back(t->icon);
			p.frac = progress_frac(t->research_time - u->building.upgrade_research_time, t->research_time);
			ci.progress.push_back(p);
		}
		if (ut_building(u) && u->building.upgrading_type) {
			auto* t = u->building.upgrading_type;
			int total = upgrade_time_cost(u->owner, t);
			console_progress p;
			p.frames.push_back(t->icon);
			p.frac = progress_frac(total - u->building.upgrade_research_time, total);
			p.label = a_string("Level ") + std::to_string(u->building.upgrading_level).c_str();
			ci.progress.push_back(p);
		}
		if (ut_building(u) && u->building.addon && !u_completed(u->building.addon)) {
			const unit_t* a = u->building.addon;
			console_progress p;
			p.frames.push_back((int)a->unit_type->id);
			p.frac = progress_frac(a->unit_type->build_time - a->remaining_build_time, a->unit_type->build_time);
			ci.progress.push_back(p);
		}
		for (auto id : u->loaded_units) {
			if (!id.raw_value) continue;
			if (const unit_t* c = get_unit(id)) ci.cargo.push_back(c);
		}
		return ci;
	}

	// cmdicons.grp pixels 1..15 index the first ticon.pcx remap row (the lit button look).
	void draw_cmd_icon(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int frame, int scale) {
		if (frame < 0 || (size_t)frame >= cmdicons_grp.frames.size() || tileset_img.wpe.size() != 256 * 4) return;
		auto& f = cmdicons_grp.frames[frame];
		auto put = [&](size_t fx, size_t fy, uint8_t v) {
			if (!v) return;
			size_t ci = ticon_remap[std::min<int>(v, 15)];
			uint32_t px = hud_rgba(tileset_img.wpe[4 * ci], tileset_img.wpe[4 * ci + 1], tileset_img.wpe[4 * ci + 2], 255);
			int X0 = x + (int)(f.offset.x + fx) * scale, Y0 = y + (int)(f.offset.y + fy) * scale;
			for (int Y = Y0; Y != Y0 + scale; ++Y) for (int X = X0; X != X0 + scale; ++X) {
				if (X >= 0 && X < w && Y >= 0 && Y < h) buf[(size_t)Y * pitch + X] = px;
			}
		};
		for (size_t fy = 0; fy != f.size.y; ++fy) {
			const uint8_t* p = f.data_container.data() + f.line_data_offset[fy];
			for (size_t fx = 0; fx < f.size.x;) {
				uint8_t c = *p++;
				if (c & 0x80) {
					fx += c & 0x7f;
				} else if (c & 0x40) {
					uint8_t v = *p++;
					for (int i = 0; i != (c & 0x3f); ++i, ++fx) put(fx, fy, v);
				} else {
					for (int i = 0; i != c; ++i, ++fx) put(fx, fy, *p++);
				}
			}
		}
	}

	void fill_rgba(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int rw, int rh, uint32_t px) {
		for (int Y = std::max(0, y); Y < std::min(h, y + rh); ++Y) {
			for (int X = std::max(0, x); X < std::min(w, x + rw); ++X) buf[(size_t)Y * pitch + X] = px;
		}
	}

	// Brood War style bar: black/olive bevelled frame and segmented "pips" like the unit hp
	// bars, filled pips green with a highlight row on top and shade on the bottom.
	void draw_bw_bar(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int bw, int bh, double frac, int pip_w) {
		fill_rgba(buf, pitch, w, h, x, y, bw, bh, hud_rgba(0, 0, 0, 255));
		fill_rgba(buf, pitch, w, h, x, y, bw, 1, hud_rgba(30, 34, 22, 255));
		fill_rgba(buf, pitch, w, h, x, y + bh - 1, bw, 1, hud_rgba(110, 122, 80, 255));
		fill_rgba(buf, pitch, w, h, x + bw - 1, y, 1, bh, hud_rgba(110, 122, 80, 255));
		int ix = x + 2, iy = y + 2, iw = bw - 4, ih = bh - 4;
		if (iw <= 0 || ih <= 0) return;
		int filled_px = (int)std::lround(iw * std::max(0.0, std::min(1.0, frac)));
		for (int px = 0; px < iw; px += pip_w + 1) {
			int pw = std::min(pip_w, iw - px);
			bool on = px < filled_px;
			uint32_t top = on ? hud_rgba(168, 252, 140, 255) : hud_rgba(44, 50, 40, 255);
			uint32_t mid = on ? hud_rgba(40, 204, 36, 255) : hud_rgba(28, 32, 26, 255);
			uint32_t bot = on ? hud_rgba(16, 112, 16, 255) : hud_rgba(18, 20, 16, 255);
			fill_rgba(buf, pitch, w, h, ix + px, iy, pw, ih, mid);
			fill_rgba(buf, pitch, w, h, ix + px, iy, pw, 1, top);
			if (ih > 2) fill_rgba(buf, pitch, w, h, ix + px, iy + ih - 1, pw, 1, bot);
		}
	}

	void prepare_panel_surface(int w, int h) {
		if (!panel_surface || panel_surface->w != w || panel_surface->h != h) {
			panel_surface = native_window_drawing::create_rgba_surface(w, h);
			panel_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
	}

	void draw_unit_console(const unit_t* u, int scale) {
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 8, gap = 4;
		auto ci = console_info_of(u);
		const int wire = 64 * scale;
		const int icon_w = 36 * scale, icon_h = 34 * scale;
		const int cargo_cell = 32 * scale + 4;
		const int bar_h = 8 * scale;
		auto text_w = [&](const a_string& s) { return (int)s.size() * cw; };

		int left_w = wire;
		for (auto& l : ci.vitals) left_w = std::max(left_w, text_w(l.text));
		int left_h = wire + (int)ci.vitals.size() * ch;

		int right_w = 0, right_h = 0;
		for (auto& l : ci.lines) right_w = std::max(right_w, text_w(l.text));
		right_h += (int)ci.lines.size() * ch;
		if (!ci.icons.empty()) {
			right_w = std::max(right_w, (int)ci.icons.size() * (icon_w + gap));
			right_h += gap + icon_h;
		}
		if (!ci.research.empty()) {
			right_w = std::max(right_w, (int)ci.research.size() * (icon_w + gap));
			right_h += gap + icon_h;
		}
		const int bar_w_min = 5 * (icon_w + gap) - gap;
		for (auto& p : ci.progress) {
			int row_w = std::max(bar_w_min, (int)p.frames.size() * (icon_w + gap) - gap);
			if (!p.label.empty()) row_w = std::max(row_w, text_w(p.label) + (p.frames.empty() ? 0 : icon_w + gap));
			right_w = std::max(right_w, row_w);
			right_h += gap + (p.frames.empty() ? ch : icon_h) + 2 + bar_h;
		}
		if (!ci.cargo.empty()) {
			right_w = std::max(right_w, (int)ci.cargo.size() * cargo_cell);
			right_h += gap + cargo_cell;
		}

		int w = pad * 3 + left_w + right_w;
		int h = pad * 2 + std::max(left_h, right_h);
		prepare_panel_surface(w, h);
		uint32_t* buf = (uint32_t*)panel_surface->lock();
		size_t pitch = panel_surface->pitch / 4;
		fill_rgba(buf, pitch, w, h, 0, 0, w, h, hud_rgba(8, 10, 16, 190));

		if (!hd_draw_wireframe(buf, pitch, w, h, pad + (left_w - wire) / 2, pad, shown_type(u), wire, vitals_of(u))) {
			draw_wireframe(buf, pitch, w, h, pad + (left_w - wire) / 2, pad, wireframe_grp, (size_t)shown_type(u)->id, scale, 1, vitals_of(u));
		}
		for (size_t i = 0; i != ci.vitals.size(); ++i) {
			auto& l = ci.vitals[i];
			hud_text(buf, pitch, w, h, pad + (left_w - text_w(l.text)) / 2, pad + wire + (int)i * ch, l.text.c_str(), l.rgb.r, l.rgb.g, l.rgb.b);
		}

		int x0 = pad * 2 + left_w, y = pad;
		for (auto& l : ci.lines) {
			hud_text(buf, pitch, w, h, x0, y, l.text.c_str(), l.rgb.r, l.rgb.g, l.rgb.b);
			y += ch;
		}
		auto draw_number = [&](int x, int y, int n) {
			a_string s = std::to_string(n).c_str();
			int tx = x + icon_w - text_w(s) - 1, ty = y + icon_h - ch + 1;
			hud_text(buf, pitch, w, h, tx + 1, ty + 1, s.c_str(), 0, 0, 0);
			hud_text(buf, pitch, w, h, tx, ty, s.c_str(), 255, 255, 255);
		};
		if (!ci.icons.empty()) {
			y += gap;
			for (size_t i = 0; i != ci.icons.size(); ++i) {
				int x = x0 + (int)i * (icon_w + gap);
				fill_rgba(buf, pitch, w, h, x, y, icon_w, icon_h, hud_rgba(0, 0, 0, 255));
				draw_cmd_icon(buf, pitch, w, h, x, y, ci.icons[i].frame, scale);
				if (ci.icons[i].number >= 0) draw_number(x, y, ci.icons[i].number);
			}
			y += icon_h;
		}
		if (!ci.research.empty()) {
			y += gap;
			for (size_t i = 0; i != ci.research.size(); ++i) {
				int x = x0 + (int)i * (icon_w + gap);
				fill_rgba(buf, pitch, w, h, x, y, icon_w, icon_h, hud_rgba(0, 0, 0, 255));
				draw_cmd_icon(buf, pitch, w, h, x, y, ci.research[i].frame, scale);
				bevel(buf, pitch, w, h, x, y, icon_w, icon_h, hud_rgba(150, 164, 108, 255), hud_rgba(44, 50, 32, 255));
				panel_tips.push_back({rect{{x, y}, {x + icon_w, y + icon_h}}, ci.research[i].name});
			}
			y += icon_h;
		}
		for (auto& p : ci.progress) {
			y += gap;
			int row_w = std::max(bar_w_min, (int)p.frames.size() * (icon_w + gap) - gap);
			if (p.frames.empty()) {
				hud_text(buf, pitch, w, h, x0, y, p.label.c_str(), 230, 230, 230);
				y += ch;
			} else {
				for (size_t i = 0; i != p.frames.size(); ++i) {
					int x = x0 + (int)i * (icon_w + gap);
					fill_rgba(buf, pitch, w, h, x, y, icon_w, icon_h, hud_rgba(0, 0, 0, 255));
					draw_cmd_icon(buf, pitch, w, h, x, y, p.frames[i], scale);
				}
				if (!p.label.empty()) hud_text(buf, pitch, w, h, x0 + icon_w + gap, y + (icon_h - ch) / 2, p.label.c_str(), 230, 230, 230);
				y += icon_h;
			}
			y += 2;
			draw_bw_bar(buf, pitch, w, h, x0, y, row_w, bar_h, p.frac, 2 * scale + 1);
			y += bar_h;
		}
		if (!ci.cargo.empty()) {
			y += gap;
			for (size_t i = 0; i != ci.cargo.size(); ++i) {
				const unit_t* c = ci.cargo[i];
				size_t id = (size_t)shown_type(c)->id;
				int x = x0 + (int)i * cargo_cell;
				if (hd_draw_wireframe(buf, pitch, w, h, x, y, shown_type(c), 32 * scale, vitals_of(c))) continue;
				if (id < group_wireframe_valid.size() && group_wireframe_valid[id]) draw_wireframe(buf, pitch, w, h, x, y, group_wireframe_grp, id, scale, 1, vitals_of(c));
				else draw_wireframe(buf, pitch, w, h, x, y, wireframe_grp, id, scale, 2, vitals_of(c));
			}
		}
		panel_surface->unlock();
	}

	// Returns false when there is nothing to show. Panel scale follows window height so
	// wireframes stay legible on large screens.
	// Window-pixel rect of the selection panel as last drawn (empty when hidden), and its
	// multi-unit grid cells, panel-relative, for clicks.
	struct panel_cell {
		rect r;
		unit_id uid;
	};
	rect panel_area_window;
	a_vector<panel_cell> panel_cells;
	struct panel_tip {
		rect r;  // panel-relative
		a_string text;
	};
	a_vector<panel_tip> panel_tips;  // hover labels on the panel (research icons)

	// The hover label of whatever panel item is under the cursor, if any.
	void draw_panel_tip() {
		if (!wnd.has_mouse_focus()) return;
		int mx = -1, my = -1;
		wnd.get_cursor_pos(&mx, &my);
		int x = mx - panel_area_window.from.x, y = my - panel_area_window.from.y;
		for (auto& t : panel_tips) {
			if (t.text.empty() || x < t.r.from.x || x >= t.r.to.x || y < t.r.from.y || y >= t.r.to.y) continue;
			draw_replay_tip(t.text, panel_area_window.from.x + (t.r.from.x + t.r.to.x) / 2, panel_area_window.from.y + t.r.from.y);
			return;
		}
	}

	// Click on the selection panel, Brood War style: a wireframe selects just that unit,
	// shift-click removes it, ctrl-click keeps only units of its type. Returns true if the
	// click was on the panel (it does not fall through to the map).
	bool selection_panel_click(int wx, int wy, bool shift, bool ctrl) {
		auto& a = panel_area_window;
		if (wx < a.from.x || wx >= a.to.x || wy < a.from.y || wy >= a.to.y) return false;
		int x = wx - a.from.x, y = wy - a.from.y;
		for (auto& c : panel_cells) {
			if (x < c.r.from.x || x >= c.r.to.x || y < c.r.from.y || y >= c.r.to.y) continue;
			unit_t* clicked = get_unit(c.uid);
			if (!clicked) break;
			if (shift) {
				current_selection_remove(clicked);
			} else if (ctrl) {
				auto type = shown_type(clicked);
				for (unit_t* u : selected_units()) if (shown_type(u) != type) current_selection_remove(u);
			} else {
				current_selection_clear();
				current_selection_add(clicked);
			}
			break;
		}
		return true;
	}

	bool draw_selection_panel() {
		panel_cells.clear();
		panel_tips.clear();
		if (!selection_panel_data_loaded) return false;
		auto units = selected_units();
		if (units.empty()) return false;
		// Group the grid by type, keeping selection order within a type.
		std::stable_sort(units.begin(), units.end(), [&](const unit_t* a, const unit_t* b) {
			return shown_type(a)->id < shown_type(b)->id;
		});
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 8;
		const int scale = window_height >= 1000 ? 2 : 1;
		if (units.size() == 1) {
			draw_unit_console(units[0], scale);
			return true;
		}
		const int per_row = 12, max_shown = 36;
		int cell = 32 * scale + 4;
		int shown = std::min((int)units.size(), max_shown);
		int rows = (shown + per_row - 1) / per_row;
		int cols = std::min(shown, per_row);
		char title[64];
		if ((int)units.size() > shown) std::snprintf(title, sizeof(title), "%d units (showing %d)", (int)units.size(), shown);
		else std::snprintf(title, sizeof(title), "%d units", (int)units.size());
		int w = std::max(pad * 2 + cols * cell, pad * 2 + (int)std::strlen(title) * cw);
		int h = pad * 2 + ch + rows * cell;
		prepare_panel_surface(w, h);
		uint32_t* buf = (uint32_t*)panel_surface->lock();
		size_t pitch = panel_surface->pitch / 4;
		for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) buf[(size_t)y * pitch + x] = hud_rgba(8, 10, 16, 190);
		hud_text(buf, pitch, w, h, pad, pad, title, 170, 180, 200);
		for (int i = 0; i != shown; ++i) {
			unit_t* u = units[i];
			int x = pad + (i % per_row) * cell, y = pad + ch + (i / per_row) * cell;
			panel_cells.push_back({rect{{x, y}, {x + cell, y + cell}}, get_unit_id(u)});
			auto pc = player_rgb(u->owner);
			for (int k = 0; k != cell - 2; ++k) {  // owner-coloured underline per cell
				int X = x + k, Y = y + cell - 3;
				if (X < w && Y < h) buf[(size_t)Y * pitch + X] = hud_rgba(pc.r, pc.g, pc.b, 255);
			}
			size_t id = (size_t)shown_type(u)->id;
			if (hd_draw_wireframe(buf, pitch, w, h, x + 1, y, shown_type(u), 32 * scale, vitals_of(u))) {
				// remastered art: the full wireframe, scaled down
			} else if (id < group_wireframe_valid.size() && group_wireframe_valid[id]) {
				draw_wireframe(buf, pitch, w, h, x + 1, y, group_wireframe_grp, id, scale, 1, vitals_of(u));
			} else {
				draw_wireframe(buf, pitch, w, h, x + 1, y, wireframe_grp, id, scale, 2, vitals_of(u));
			}
		}
		panel_surface->unlock();
		return true;
	}

	// ---- Unit value and losses (graphs) ----
	// Resource value of a unit: mineral + gas cost, with the units.dat quirks corrected:
	// Zerglings and Scourge come two per egg, morphed units include their base unit, and
	// Archons are listed at 0/0.
	int unit_type_value(const unit_type_t* ut) const {
		auto cost = [&](UnitTypes id) {
			auto* t = get_unit_type(id);
			return t->mineral_cost + t->gas_cost;
		};
		int v = ut->mineral_cost + ut->gas_cost;
		switch (ut->id) {
		case UnitTypes::Zerg_Zergling:
		case UnitTypes::Zerg_Scourge: return v / 2;
		case UnitTypes::Zerg_Lurker: return v + cost(UnitTypes::Zerg_Hydralisk);
		case UnitTypes::Zerg_Guardian:
		case UnitTypes::Zerg_Devourer: return v + cost(UnitTypes::Zerg_Mutalisk);
		case UnitTypes::Protoss_Archon: return 2 * cost(UnitTypes::Protoss_High_Templar);
		case UnitTypes::Protoss_Dark_Archon: return 2 * cost(UnitTypes::Protoss_Dark_Templar);
		case UnitTypes::Zerg_Broodling: return 0;
		default: return v;
		}
	}

	// Eggs and cocoons are valued as what they will hatch into (both Zerglings of a pair).
	const unit_type_t* egg_target(const unit_t* u) const {
		if (unit_is_egg(u) && !u->build_queue.empty()) return u->build_queue.front();
		return nullptr;
	}

	// What a unit counts as in the value graphs: an egg is classed by what it will hatch into.
	const unit_type_t* valued_type(const unit_t* u) const {
		const unit_type_t* t = egg_target(u);
		return t ? t : u->unit_type;
	}
	// Overlords provide supply, like Supply Depots and Pylons (buildings, which the value
	// graphs leave out), so they are not counted as units at all.
	bool unit_counts_as_unit(const unit_t* u) const {
		return valued_type(u)->id != UnitTypes::Zerg_Overlord;
	}
	// Army value also excludes workers.
	bool unit_counts_as_army(const unit_t* u) const {
		const unit_type_t* t = valued_type(u);
		return unit_counts_as_unit(u) && !ut_worker(t) && t->id != UnitTypes::Zerg_Larva;
	}
	int unit_value(const unit_t* u) const {
		if (auto* t = egg_target(u)) return unit_type_value(t) * (ut_two_units_in_one_egg(t) ? 2 : 1);
		return unit_type_value(u->unit_type);
	}
	int unit_supply_half(const unit_t* u) const {
		if (auto* t = egg_target(u)) return (int)t->supply_required.raw_value * (ut_two_units_in_one_egg(t) ? 2 : 1);
		return (int)u->unit_type->supply_required.raw_value;
	}

	struct death_event {
		int owner, supply_half, value;
	};
	// Keyed by (frame, unit id) so re-simulating after a rewind records each death once.
	a_map<std::pair<int, uint32_t>, death_event> unit_deaths;

	// A loss is a completed non-building unit dying. Refunded cancels never get here
	// complete, hallucinations cost nothing, and units consumed by their own action
	// (drone into extractor, launched nuke, scarab, mine, scanner) are not losses.
	void record_unit_loss(const unit_t* u) {
		if (u->owner < 0 || u->owner >= 8) return;
		if (ut_building(u) || u_hallucination(u)) return;
		if (!u_completed(u) && !unit_is_egg(u)) return;
		if (u->user_action_flags & 4) return;
		switch (u->unit_type->id) {
		case UnitTypes::Protoss_Scarab:
		case UnitTypes::Terran_Vulture_Spider_Mine:
		case UnitTypes::Terran_Nuclear_Missile:
		case UnitTypes::Spell_Scanner_Sweep:
		case UnitTypes::Spell_Disruption_Web:
		case UnitTypes::Spell_Dark_Swarm:
			return;
		default: break;
		}
		int value = unit_value(u), supply = unit_supply_half(u);
		if (value <= 0 && supply <= 0) return;
		unit_deaths[{st.current_frame, (uint32_t)get_unit_id(u).raw_value}] = {u->owner, supply, value};
	}

	void on_kill_unit(unit_t* u) override {
		record_unit_loss(u);
		observer_on_kill(u);
	}

	// ---- Auto observer ('o') ----
	// Two layers, as in esports auto-directors: choosing the scene runs on game time
	// (every 8 frames), moving the camera runs on real time, so motion feels the same at
	// any playback speed.
	//
	// Interest of a view-sized window (observer_window, on the interest grid below):
	//  - recent damage: hp + shields lost, from comparing units between ticks plus the hp
	//    left on units that die, with a 2 game-second half-life;
	//  - a fight: the second-largest side's army supply in the window;
	//  - a fight about to happen: the same, counting armies up to about a view away;
	//  - presence: total army supply;
	//  - a quiet-time part: scouting workers early on and buildings under construction.
	//    It wears off the longer a shot lasts and counts less where the camera has been
	//    recently, so a quiet game tours the bases instead of staring at one spot.
	// Choice (observer_retarget): a shot lasts at least observer_min_shot_s of real time
	// (less once its fight is over); another place must beat it by 80% for 1.5 game
	// seconds. Motion (observer_step): follow the shot's weighted centre through a dead zone
	// with a critically damped spring; cut instead when the target is most of a view away,
	// as a human observer clicks the minimap. It steps back for a few seconds after a
	// manual camera move.
	bool auto_observer = false;
	struct damage_event {
		xy pos;
		int amount;
		int frame;
	};
	a_vector<damage_event> observer_damage;
	a_vector<std::pair<uint32_t, int>> observer_last_hp = a_vector<std::pair<uint32_t, int>>(2048);
	int observer_last_frame = -1;
	int observer_tick_frame = -1000;
	bool observer_has_target = false;
	xy observer_target;                  // the shown scene's centre, updated on game ticks
	double observer_sx = 0, observer_sy = 0;  // observer_target smoothed in real time
	double observer_cx = 0, observer_cy = 0, observer_vx = 0, observer_vy = 0;
	bool observer_cut_pending = false;
	xy observer_last_screen_pos;
	std::chrono::high_resolution_clock::time_point observer_manual_until, observer_last_switch, observer_last_update;
	static constexpr double observer_min_shot_s = 2.5;

	// Playback speed (1 = Fastest); 0 while paused.
	double observer_speed() const { return is_paused ? 0.0 : (double)game_speed.raw_value / 256.0; }

	int unit_hp_total(const unit_t* u) const {
		return (int)u->hp.integer_part() + (u->unit_type->has_shield ? (int)u->shield_points.integer_part() : 0);
	}

	void observer_on_kill(const unit_t* u) {
		if (u->owner < 0 || u->owner >= 8 || u_hallucination(u)) return;
		auto uid = get_unit_id(u);
		auto& e = observer_last_hp[uid.index() & 2047];
		int remaining = e.first == (uint32_t)uid.raw_value ? e.second : unit_hp_total(u);
		observer_damage.push_back({u->sprite->position, std::max(remaining, 20), st.current_frame});
	}

	void observer_scan_damage() {
		if (st.current_frame == observer_last_frame) return;
		bool rebase = observer_last_frame < 0 || st.current_frame < observer_last_frame || st.current_frame - observer_last_frame > 240;
		if (rebase) observer_damage.clear();
		for (int i = 0; i != 8; ++i) {
			for (unit_t* u : ptr(st.player_units[i])) {
				auto uid = get_unit_id(u);
				auto& e = observer_last_hp[uid.index() & 2047];
				int hp = unit_hp_total(u);
				if (!rebase && e.first == (uint32_t)uid.raw_value && hp < e.second && !u_hallucination(u)) {
					observer_damage.push_back({u->sprite->position, e.second - hp, st.current_frame});
				}
				e = {(uint32_t)uid.raw_value, hp};
			}
		}
		int horizon = st.current_frame - 150;
		observer_damage.erase(std::remove_if(observer_damage.begin(), observer_damage.end(),
			[&](const damage_event& d) { return d.frame < horizon || d.frame > st.current_frame; }), observer_damage.end());
		observer_last_frame = st.current_frame;
	}

	// Interest heatmap: a grid of 128 px cells. Each observer tick rebuilds per-cell army
	// supply per player, decayed damage, scouts and construction sites, takes window sums
	// with summed-area tables, and keeps a per-window score smoothed over ~1 game second,
	// so decisions follow sustained interest rather than single volleys.
	static constexpr int observer_cell = 128;
	int observer_gw = 0, observer_gh = 0;
	a_vector<double> observer_smooth_action;  // per window centre cell
	int observer_smooth_frame = -1;

	struct observer_interest {
		double action = 0;  // damage, fights and armies
		double quiet = 0;   // scouting and construction
		xy centre;
	};

	struct observer_grid {
		int w = 0, h = 0;
		// summed-area tables, (w+1) x (h+1)
		std::array<a_vector<int>, 8> supply;
		a_vector<double> damage, wx, wy, wsum;
		a_vector<int> scouts, builds;
		size_t at(int x, int y) const { return (size_t)y * (w + 1) + x; }
		template<typename T>
		static T rect_sum(const a_vector<T>& t, const observer_grid& g, int x0, int y0, int x1, int y1) {
			// inclusive cell range, clipped
			x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(g.w - 1, x1); y1 = std::min(g.h - 1, y1);
			if (x0 > x1 || y0 > y1) return T();
			return t[g.at(x1 + 1, y1 + 1)] - t[g.at(x0, y1 + 1)] - t[g.at(x1 + 1, y0)] + t[g.at(x0, y0)];
		}
	};

	template<typename T>
	static void observer_integrate(a_vector<T>& t, int w, int h) {
		for (int y = 1; y <= h; ++y) for (int x = 1; x <= w; ++x) {
			size_t i = (size_t)y * (w + 1) + x;
			t[i] += t[i - 1] + t[i - (w + 1)] - t[i - (w + 1) - 1];
		}
	}

	observer_grid observer_build_grid() const {
		observer_grid g;
		g.w = std::max(1, ((int)game_st.map_width + observer_cell - 1) / observer_cell);
		g.h = std::max(1, ((int)game_st.map_height + observer_cell - 1) / observer_cell);
		size_t n = (size_t)(g.w + 1) * (g.h + 1);
		for (auto& v : g.supply) v.assign(n, 0);
		g.damage.assign(n, 0); g.wx.assign(n, 0); g.wy.assign(n, 0); g.wsum.assign(n, 0);
		g.scouts.assign(n, 0); g.builds.assign(n, 0);
		auto cell = [&](xy p) {
			int cx = std::max(0, std::min(g.w - 1, p.x / observer_cell)), cy = std::max(0, std::min(g.h - 1, p.y / observer_cell));
			return g.at(cx + 1, cy + 1);
		};
		bool early = st.current_frame < 7500;  // about 5 game minutes: scouting matters
		for (int i = 0; i != 8; ++i) {
			xy home = game_st.start_locations[i];
			for (unit_t* u : ptr(st.player_units[i])) {
				if (u_hallucination(u) || (u->status_flags & unit_t::status_flag_loaded)) continue;
				xy p = u->sprite->position;
				size_t c = cell(p);
				double w = 0;
				if (ut_building(u)) {
					if (u_completed(u) || unit_is(u, UnitTypes::Zerg_Creep_Colony)) continue;
					++g.builds[c];
					w = 1;
				} else if (ut_worker(u)) {
					if (!early || std::abs(p.x - home.x) + std::abs(p.y - home.y) <= 1600) continue;
					++g.scouts[c];
					w = 4;
				} else {
					if (!u_completed(u)) continue;
					int s = (int)u->unit_type->supply_required.raw_value;
					if (s <= 0) continue;
					g.supply[i][c] += s;
					w = s;
				}
				g.wx[c] += p.x * w; g.wy[c] += p.y * w; g.wsum[c] += w;
			}
		}
		for (auto& d : observer_damage) {
			double w = d.amount * std::exp2(-(st.current_frame - d.frame) / 48.0);
			size_t c = cell(d.pos);
			g.damage[c] += w;
			g.wx[c] += d.pos.x * w * 0.5; g.wy[c] += d.pos.y * w * 0.5; g.wsum[c] += w * 0.5;
		}
		for (auto& v : g.supply) observer_integrate(v, g.w, g.h);
		observer_integrate(g.damage, g.w, g.h); observer_integrate(g.wx, g.w, g.h);
		observer_integrate(g.wy, g.w, g.h); observer_integrate(g.wsum, g.w, g.h);
		observer_integrate(g.scouts, g.w, g.h); observer_integrate(g.builds, g.w, g.h);
		return g;
	}

	// Instantaneous interest of the view-sized window centred on cell (cx, cy).
	observer_interest observer_window(const observer_grid& g, int cx, int cy) const {
		int hw = std::max(1, (int)(view_width * 2 / 5 + observer_cell / 2) / observer_cell);
		int hh = std::max(1, (int)(view_height * 2 / 5 + observer_cell / 2) / observer_cell);
		int nw = hw + std::max(1, (int)view_width / observer_cell), nh = hh + std::max(1, (int)view_height / observer_cell);
		std::array<int, 8> side, near;
		for (int i = 0; i != 8; ++i) {
			side[i] = observer_grid::rect_sum(g.supply[i], g, cx - hw, cy - hh, cx + hw, cy + hh);
			near[i] = observer_grid::rect_sum(g.supply[i], g, cx - nw, cy - nh, cx + nw, cy + nh);
		}
		// About to meet: the largest army in the window, against the strongest other army
		// within about a view of it (not the midpoint between two distant armies).
		int top = (int)(std::max_element(side.begin(), side.end()) - side.begin());
		int enemy_near = 0;
		for (int i = 0; i != 8; ++i) if (i != top) enemy_near = std::max(enemy_near, near[i]);
		int approaching = std::min(side[top], enemy_near);
		std::sort(side.begin(), side.end(), std::greater<int>());
		int total = 0;
		for (int v : side) total += v;
		double damage = observer_grid::rect_sum(g.damage, g, cx - hw, cy - hh, cx + hw, cy + hh);
		int scouts = observer_grid::rect_sum(g.scouts, g, cx - hw, cy - hh, cx + hw, cy + hh);
		int builds = observer_grid::rect_sum(g.builds, g, cx - hw, cy - hh, cx + hw, cy + hh);
		double wsum = observer_grid::rect_sum(g.wsum, g, cx - hw, cy - hh, cx + hw, cy + hh);
		observer_interest r;
		r.action = damage / 15.0 + 2.0 * (side[1] / 2.0) + 1.0 * (approaching / 2.0) + 0.15 * (total / 2.0);
		r.quiet = 1.5 * std::min(scouts, 2) + 0.5 * std::min(builds, 3);
		if (wsum > 0) {
			r.centre = xy((int)(observer_grid::rect_sum(g.wx, g, cx - hw, cy - hh, cx + hw, cy + hh) / wsum),
			              (int)(observer_grid::rect_sum(g.wy, g, cx - hw, cy - hh, cx + hw, cy + hh) / wsum));
		} else {
			r.centre = xy(cx * observer_cell + observer_cell / 2, cy * observer_cell + observer_cell / 2);
		}
		return r;
	}

	int observer_challenger_ticks = 0;
	xy observer_challenger;
	std::chrono::high_resolution_clock::time_point observer_challenger_since;
	double observer_shot_peak = 0;
	struct observer_visit {
		xy pos;
		std::chrono::high_resolution_clock::time_point until;
	};
	a_vector<observer_visit> observer_visits;

	double observer_recency(xy c, std::chrono::high_resolution_clock::time_point now) const {
		for (auto& v : observer_visits) {
			if (now < v.until && std::abs(v.pos.x - c.x) < (int)view_width && std::abs(v.pos.y - c.y) < (int)view_height) return 0.3;
		}
		return 1.0;
	}

	void observer_switch_to(xy target, std::chrono::high_resolution_clock::time_point now, double action = 0) {
		if (observer_has_target) observer_visits.push_back({observer_target, now + std::chrono::seconds(20)});
		observer_visits.erase(std::remove_if(observer_visits.begin(), observer_visits.end(),
			[&](const observer_visit& v) { return v.until <= now; }), observer_visits.end());
		observer_target = target;
		observer_sx = target.x;
		observer_sy = target.y;
		observer_has_target = true;
		observer_last_switch = now;
		observer_challenger_ticks = 0;
		observer_shot_peak = action;
		observer_cut_pending = true;
	}

	void observer_retarget(std::chrono::high_resolution_clock::time_point now) {
		auto g = observer_build_grid();
		size_t cells = (size_t)g.w * g.h;
		// Smooth each window's action score over ~1 game second (restart after a seek).
		bool restart = observer_gw != g.w || observer_gh != g.h || observer_smooth_frame < 0 ||
			st.current_frame < observer_smooth_frame || st.current_frame - observer_smooth_frame > 240;
		// Rises within ~1/4 game second, so a new fight registers at once; falls over ~1 s,
		// so a lull does not drop the scene.
		int dframes = st.current_frame - observer_smooth_frame;
		double k_up = restart ? 1.0 : 1.0 - std::exp(-dframes / 6.0);
		double k_down = restart ? 1.0 : 1.0 - std::exp(-dframes / 24.0);
		if (restart) { observer_gw = g.w; observer_gh = g.h; observer_smooth_action.assign(cells, 0); }
		observer_smooth_frame = st.current_frame;
		a_vector<observer_interest> win(cells);
		for (int cy = 0; cy != g.h; ++cy) for (int cx = 0; cx != g.w; ++cx) {
			size_t i = (size_t)cy * g.w + cx;
			win[i] = observer_window(g, cx, cy);
			double d = win[i].action - observer_smooth_action[i];
			observer_smooth_action[i] += d * (d > 0 ? k_up : k_down);
		}
		double shot_s = observer_has_target ? std::chrono::duration<double>(now - observer_last_switch).count() : 0;
		double best = 0, current = -1, current_action = 0, best_action = 0;
		xy best_centre, current_centre = observer_target;
		for (int cy = 0; cy != g.h; ++cy) for (int cx = 0; cx != g.w; ++cx) {
			size_t i = (size_t)cy * g.w + cx;
			xy c(cx * observer_cell + observer_cell / 2, cy * observer_cell + observer_cell / 2);
			double a = observer_smooth_action[i];
			double sc = a + win[i].quiet * observer_recency(c, now);
			if (sc > best) { best = sc; best_centre = win[i].centre; best_action = a; }
			// The shown scene is the best window within half a view of it, so it is judged
			// like its challengers and tracked to where its action has moved. Its quiet part
			// wears off over ~10 s, so quiet time moves on.
			if (observer_has_target && std::abs(c.x - observer_target.x) <= (int)view_width / 2 && std::abs(c.y - observer_target.y) <= (int)view_height / 2) {
				double cur = a + win[i].quiet * std::exp(-shot_s / 10.0);
				if (cur > current) { current = cur; current_centre = win[i].centre; current_action = a; }
			}
		}
		if (!observer_has_target) {
			if (best > 0.3) observer_switch_to(best_centre, now, best_action);
			else {
				// Nothing to show yet (the opening): frame the first player's base.
				for (int i = 0; i != 8; ++i) {
					if (!st.players[i].initially_active) continue;
					observer_switch_to(game_st.start_locations[i], now, 0);
					break;
				}
			}
			return;
		}
		observer_shot_peak = std::max(observer_shot_peak, current_action);
		// Follow the shown scene (smoothed again in real time by observer_step).
		if (current > 0) {
			observer_target.x += (current_centre.x - observer_target.x) / 2;
			observer_target.y += (current_centre.y - observer_target.y) / 2;
		}
		bool elsewhere = std::abs(best_centre.x - observer_target.x) > (int)view_width / 2 || std::abs(best_centre.y - observer_target.y) > (int)view_height / 2;
		if (elsewhere && best > current * 1.8 + 0.5) {
			bool same = observer_challenger_ticks > 0 && std::abs(best_centre.x - observer_challenger.x) < (int)view_width / 2 && std::abs(best_centre.y - observer_challenger.y) < (int)view_height / 2;
			if (!same) observer_challenger_since = now;
			observer_challenger_ticks = same ? observer_challenger_ticks + 1 : 1;
			observer_challenger = best_centre;
		} else {
			observer_challenger_ticks = 0;
		}
		// A challenger must lead for 2 ticks and 0.75 game seconds (at most 0.25 s of real
		// time). Minimum shot lengths: a fight holds the camera for observer_min_shot_s, but
		// one that has died down, a quiet shot, or one dwarfed by a new fight gives way after
		// 1-1.5 s; moving on from one quiet place to another waits 5 s, so a tour does not
		// flicker.
		bool over = observer_shot_peak > 4 && current_action < observer_shot_peak * 0.25;
		bool quiet_shot = observer_shot_peak < 2;
		bool challenger_action = best_action >= 2;
		bool dwarfed = best > current * 2.5 + 2;
		double min_shot = !challenger_action ? 5.0 : dwarfed ? 1.0 : (over || quiet_shot) ? 1.5 : observer_min_shot_s;
		double hold_s = std::min(0.25, 0.75 * 42.0 / 1000.0 / std::max(0.05, observer_speed()));
		bool held = observer_challenger_ticks >= 2 && std::chrono::duration<double>(now - observer_challenger_since).count() >= hold_s;
		if (held && shot_s >= min_shot) observer_switch_to(observer_challenger, now, best_action);
	}

	void observer_step(std::chrono::high_resolution_clock::time_point now, bool manual_move) {
		double dt = std::chrono::duration<double>(now - observer_last_update).count();
		observer_last_update = now;
		if (!auto_observer) return;
		if (manual_move) observer_manual_until = now + std::chrono::seconds(4);
		observer_scan_damage();
		if (st.current_frame - observer_tick_frame >= 8 || st.current_frame < observer_tick_frame) {
			observer_tick_frame = st.current_frame;
			observer_retarget(now);
		}
		if (!observer_has_target || now < observer_manual_until) {
			observer_vx = observer_vy = 0;
			return;
		}
		if (screen_pos != observer_last_screen_pos) {
			observer_cx = screen_pos.x + view_width / 2.0;
			observer_cy = screen_pos.y + view_height / 2.0;
			observer_vx = observer_vy = 0;
		}
		// Where the camera can actually centre (the view stays inside the map).
		double min_cx = view_width / 2.0, max_cx = std::max(min_cx, game_st.map_width - view_width / 2.0);
		double min_cy = view_height / 2.0, max_cy = std::max(min_cy, game_st.map_height - view_height / 2.0);
		// Smooth the scene centre over ~3/4 game second, kept between 0.1 and 0.3 s of real
		// time: steady at slow playback, still keeping up with a fight at high speed.
		{
			double tau = std::max(0.1, std::min(0.3, 0.75 * 42.0 / 1000.0 / std::max(0.05, observer_speed())));
			double k = 1.0 - std::exp(-std::max(0.0, std::min(dt, 0.25)) / tau);
			observer_sx += (observer_target.x - observer_sx) * k;
			observer_sy += (observer_target.y - observer_sy) * k;
		}
		double tx = std::max(min_cx, std::min(max_cx, observer_sx));
		double ty = std::max(min_cy, std::min(max_cy, observer_sy));
		double dx = tx - observer_cx, dy = ty - observer_cy;
		bool far = std::abs(dx) > 0.8 * view_width || std::abs(dy) > 0.8 * view_height;
		if (far && (observer_cut_pending || std::abs(dx) > 1.5 * view_width || std::abs(dy) > 1.5 * view_height)) {
			observer_cx = tx;  // cut, like a human observer clicking the minimap
			observer_cy = ty;
			observer_vx = observer_vy = 0;
			observer_panning = false;
		} else {
			// Dead zone: start moving when the target leaves the middle 30% of the view,
			// settle once it is back within 5% of centre.
			bool outside = std::abs(dx) > view_width * 0.15 || std::abs(dy) > view_height * 0.15;
			bool centred = std::abs(dx) < view_width * 0.05 && std::abs(dy) < view_height * 0.05;
			if (outside) observer_panning = true;
			if (centred) observer_panning = false;
			double t = std::max(0.0, std::min(dt, 0.1));
			const double omega = 2.0 / 0.25;  // critically damped spring, ~0.25 s to settle
			double ax = observer_panning ? omega * omega * dx : 0, ay = observer_panning ? omega * omega * dy : 0;
			observer_vx += (ax - 2 * omega * observer_vx) * t;
			observer_vy += (ay - 2 * omega * observer_vy) * t;
			double cap = 1.5 * std::max(view_width, view_height);  // px per second
			double speed = std::hypot(observer_vx, observer_vy);
			if (speed > cap) { observer_vx *= cap / speed; observer_vy *= cap / speed; }
			observer_cx += observer_vx * t;
			observer_cy += observer_vy * t;
		}
		observer_cut_pending = false;
		observer_cx = std::max(min_cx, std::min(max_cx, observer_cx));
		observer_cy = std::max(min_cy, std::min(max_cy, observer_cy));
		screen_pos = xy((int)std::lround(observer_cx - view_width / 2.0), (int)std::lround(observer_cy - view_height / 2.0));
	}
	bool observer_panning = false;

	// ---- Graph panel ('g'): income and supply over game time ----
	// Sampled once per game second as frames are simulated (playback, seeking and rewinds all
	// pass through record_graph_sample), so the graphs cover the part of the game simulated
	// so far. The x axis spans the whole replay, or with time zoom ([ ] or the mouse wheel
	// over the panel) only the last N game seconds up to the current frame. Income is the
	// gathering rate per minute, smoothed over roughly the last 7-8 game seconds.
	struct graph_player_sample {
		int minerals_gathered = 0, gas_gathered = 0, supply = 0, army = 0, workers = 0;
		int value_total = 0, value_army = 0;   // resource value of units owned
		int lost_supply_half = 0, lost_value = 0;  // cumulative units lost
	};
	struct graph_sample {
		bool valid = false;
		std::array<graph_player_sample, 8> players;
	};
	static constexpr int graph_sample_interval = 24;  // frames; ~1 game second at Fastest
	a_vector<graph_sample> graph_samples;
	bool show_graphs = false;
	int graph_tab = 1;  // unit value (supply with 'r' off)
	// Time zoom: index into graph_window_seconds; 0 = whole game. Starts at the last 5 min.
	static constexpr int graph_window_count = 7;
	int graph_window = 2;
	static int graph_window_seconds(int index) {
		static const int s[graph_window_count] = {0, 600, 300, 120, 60, 30, 15};
		return s[std::max(0, std::min(graph_window_count - 1, index))];
	}
	void step_graph_window(int dir) {
		graph_window = std::max(0, std::min(graph_window_count - 1, graph_window + dir));
	}
	static constexpr int graph_tab_count = 5;
	bool graph_value_mode = true;  // 'r': resource value <-> supply (workers unaffected)
	bool graph_derivative = false;  // 'd': plot the rate of change per minute instead
	bool graph_difference = false;  // 'f': first player minus second instead of a line each
	// 'm': minimized mode. The graphs then stay up as a compact panel in the top-right
	// corner of the view, and 'g' (or a click) maximizes / minimizes instead of closing.
	bool graph_minimized = false;
	bool graph_expanded = false;  // minimized mode: the full panel is showing for now
	bool graphs_visible() const { return show_graphs || graph_minimized; }
	bool graph_mini() const { return graph_minimized && !graph_expanded; }
	bool graph_full() const { return graphs_visible() && !graph_mini(); }
	std::unique_ptr<native_window_drawing::surface> graph_surface;
	rect graph_area_window;                                // panel rect in window pixels
	std::array<rect, graph_tab_count> graph_tab_rects{};   // tab rects, panel-relative

	bool graph_arrows_switch_tabs() const { return graph_full(); }

	void record_graph_sample() {
		if (st.current_frame % graph_sample_interval != 0) return;
		size_t index = (size_t)st.current_frame / graph_sample_interval;
		if (graph_samples.size() <= index) graph_samples.resize(index + 1);
		graph_sample& s = graph_samples[index];
		s.valid = true;
		for (auto& row : hud_rows()) {
			auto& p = s.players[row.player];
			p.minerals_gathered = st.total_minerals_gathered[row.player];
			p.gas_gathered = st.total_gas_gathered[row.player];
			p.supply = row.supply;
			p.army = row.army;
			p.workers = row.workers;
			p.value_total = p.value_army = 0;
			// Units in production exist as incomplete units (Terran/Protoss training, morphs)
			// or eggs, and count; queued ones do not exist yet. Workers count in the total but
			// not the army; Overlords count in neither.
			for (unit_t* u : ptr(st.player_units[row.player])) {
				if (ut_building(u) || u_hallucination(u) || !unit_counts_as_unit(u)) continue;
				int v = unit_value(u);
				p.value_total += v;
				if (unit_counts_as_army(u)) p.value_army += v;
			}
			p.lost_supply_half = p.lost_value = 0;
		}
		for (auto& d : unit_deaths) {
			if (d.first.first > st.current_frame) break;
			auto& p = s.players[d.second.owner];
			p.lost_supply_half += d.second.supply_half;
			p.lost_value += d.second.value;
		}
	}

	const char* graph_tab_name(int tab) const {
		static const char* supply_names[graph_tab_count] = {"Income", "Supply", "Army supply", "Workers", "Supply lost"};
		static const char* value_names[graph_tab_count] = {"Income", "Unit value", "Army value", "Workers", "Value lost"};
		return graph_value_mode ? value_names[tab] : supply_names[tab];
	}

	// Value plotted for one player at sample index i (the second series is gas income).
	// Income, and every series in derivative mode, is a per-minute rate: per-second deltas
	// averaged with a Hann window, over 12 s for income (similar to a 7-8 s box average) and
	// 24 s for derivatives. Plain box windows jitter, because mining arrives in 8-mineral
	// trips that step in and out of them.
	static constexpr int income_window_s = 12, derivative_window_s = 24;
	template<typename level_F>
	double smoothed_rate(level_F&& level, size_t i, int window) const {
		double sum = 0, wsum = 0;
		for (int k = 0; k != window && (size_t)k < i; ++k) {
			size_t j = i - k;
			if (!graph_samples[j].valid || !graph_samples[j - 1].valid) continue;
			double wk = std::sin(3.14159265358979 * (k + 0.5) / window);
			wk *= wk;
			sum += wk * (level(j) - level(j - 1));
			wsum += wk;
		}
		return wsum > 0 ? sum / wsum * (60.0 * 1000.0 / 42.0 / graph_sample_interval) : 0;
	}

	double graph_value(int tab, size_t i, int player, bool gas) const {
		auto level = [&](size_t j) { return graph_level(tab, j, player, gas); };
		auto shown = [&](size_t j) { return tab == 0 ? smoothed_rate(level, j, income_window_s) : level(j); };
		return graph_derivative ? smoothed_rate(shown, i, derivative_window_s) : shown(i);
	}

	// Raw level: cumulative minerals/gas gathered for income, else the tab's quantity.
	double graph_level(int tab, size_t i, int player, bool gas) const {
		const auto& p = graph_samples[i].players[player];
		switch (tab) {
		case 0: return gas ? p.gas_gathered : p.minerals_gathered;
		case 1: return graph_value_mode ? p.value_total : p.supply;
		case 2: return graph_value_mode ? p.value_army : p.army;
		case 3: return p.workers;
		default: return graph_value_mode ? p.lost_value : p.lost_supply_half / 2.0;
		}
	}

	// Smallest of 1, 2, 2.5, 5 x 10^k that is >= v (v > 0).
	static double nice_step(double v) {
		double mag = std::pow(10.0, std::floor(std::log10(v)));
		for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) if (m * mag >= v - 1e-9) return m * mag;
		return 10 * mag;
	}

	// A dash pattern that runs on along a polyline: `on` pixels drawn, then `off` skipped.
	// `joined`: the next segment starts where the last one ended, so its first pixel is
	// not counted twice.
	struct dash_pattern {
		int on = 0, off = 0;
		int phase = 0;
		bool joined = false;
	};

	// Solid, or dashed by `dash` (which then carries on to the next call).
	void graph_line(uint32_t* buf, size_t pitch, int w, int h, int x0, int y0, int x1, int y1, uint32_t px, int thickness, dash_pattern* dash = nullptr) {
		int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
		int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
		bool first = true;
		for (;;) {
			bool draw = true;
			if (dash && dash->on > 0 && dash->off > 0) {
				if (first && dash->joined) draw = false;  // drawn (or skipped) as the last segment's end
				else draw = dash->phase++ % (dash->on + dash->off) < dash->on;
			}
			first = false;
			if (draw) for (int ty = 0; ty != thickness; ++ty) for (int tx = 0; tx != thickness; ++tx) {
				int X = x0 + tx, Y = y0 + ty;
				if (X >= 0 && X < w && Y >= 0 && Y < h) buf[(size_t)Y * pitch + X] = px;
			}
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
		if (dash) dash->joined = true;
	}

	static a_string format_game_time(int frame) {
		int secs = (int)(frame * 42 / 1000);
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
		return buf;
	}

	// One plotted line: player a's value, or a minus b in difference mode ('f'). The line
	// takes the colour of whoever is doing better: a's where it is >= 0 and b's below zero,
	// reversed for losses.
	struct graph_series {
		int a = 0, b = -1;
		bool gas = false;
		uint32_t px_pos = 0, px_neg = 0;
	};

	a_vector<graph_series> graph_series_list(const a_vector<hud_row>& rows) const {
		a_vector<graph_series> out;
		auto colour = [&](const hud_row& r, bool gas) {
			return gas ? hud_rgba(r.r / 2 + 30, r.g / 2 + 30, r.b / 2 + 30, 255) : hud_rgba(r.r, r.g, r.b, 255);
		};
		// Gas (income tab only) first, so the mineral lines draw over it.
		for (int g = graph_tab == 0 ? 1 : 0; g >= 0; --g) {
			if (graph_difference && rows.size() >= 2) {
				// Losses: more is worse, so the line takes the colour of whoever lost less.
				bool lower_is_better = graph_tab == 4;
				auto pos = colour(rows[lower_is_better ? 1 : 0], g == 1), neg = colour(rows[lower_is_better ? 0 : 1], g == 1);
				out.push_back({rows[0].player, rows[1].player, g == 1, pos, neg});
			} else {
				for (auto& r : rows) out.push_back({r.player, -1, g == 1, colour(r, g == 1), colour(r, g == 1)});
			}
		}
		return out;
	}

	double graph_series_value(const graph_series& s, size_t i) const {
		double v = graph_value(graph_tab, i, s.a, s.gas);
		return s.b >= 0 ? v - graph_value(graph_tab, i, s.b, s.gas) : v;
	}

	// Full panel centred over the game view, or with 'm' a compact one in its top-right
	// corner (no tabs or legend; keys and the mouse wheel still work, a click expands it).
	void draw_graph_panel() {
		const int cw = hud_font::cell_w, ch = hud_font::cell_h;
		const bool mini = graph_mini();
		const int pad = mini ? 8 : 12;
		int w, h;
		if (mini) {
			w = std::max(340, std::min(viewport_w / 4, 480));
			h = w * 2 / 3;
		} else {
			w = std::max(320, std::min(viewport_w - 80, 1400));
			h = std::max(240, std::min(window_height * 3 / 5, 700));
		}
		if (!graph_surface || graph_surface->w != w || graph_surface->h != h) {
			graph_surface = native_window_drawing::create_rgba_surface(w, h);
			graph_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)graph_surface->lock();
		size_t pitch = graph_surface->pitch / 4;
		fill_rgba(buf, pitch, w, h, 0, 0, w, h, hud_rgba(8, 10, 16, mini ? 190 : 215));

		auto rows = hud_rows();
		bool difference = graph_difference && rows.size() >= 2;
		int win_s = graph_window_seconds(graph_window);
		char range[24];
		if (win_s) std::snprintf(range, sizeof(range), "last %d:%02d", win_s / 60, win_s % 60);
		else std::snprintf(range, sizeof(range), "whole game");

		int ly;  // y of the last header line
		if (mini) {
			graph_tab_rects.fill(rect{});
			char title[96];
			std::snprintf(title, sizeof(title), "%s%s%s%s%s", graph_tab_name(graph_tab), graph_derivative ? " /min" : "",
				difference ? " diff" : "", win_s ? ", " : "", win_s ? range : "");
			hud_text(buf, pitch, w, h, pad, pad, title, 230, 230, 240);
			const char* hint = "g: maximize";
			hud_text(buf, pitch, w, h, w - pad - (int)std::strlen(hint) * cw, pad, hint, 120, 130, 150);
			ly = pad;
		} else {
			// Tabs
			int tx = pad;
			for (int t = 0; t != graph_tab_count; ++t) {
				char label[48];
				std::snprintf(label, sizeof(label), "%d %s", t + 1, graph_tab_name(t));
				int tw = (int)std::strlen(label) * cw + 16;
				graph_tab_rects[t] = rect{{tx, pad}, {tx + tw, pad + ch + 8}};
				bool active = t == graph_tab;
				fill_rgba(buf, pitch, w, h, tx, pad, tw, ch + 8, active ? hud_rgba(60, 70, 100, 255) : hud_rgba(28, 32, 44, 255));
				hud_text(buf, pitch, w, h, tx + 8, pad + 4, label, active ? 255 : 160, active ? 255 : 170, active ? 255 : 190);
				tx += tw + 6;
			}
			const char* keys = graph_minimized ? "[ ] zoom  g minimize  m unpin" : "[ ] zoom  m minimize  esc close";
			hud_text(buf, pitch, w, h, w - pad - (int)std::strlen(keys) * cw, pad + 4, keys, 120, 130, 150);

			// Legend, and the display modes on the right
			ly = pad + ch + 16;
			int lx = pad;
			if (difference) {
				a_string a = rows[0].name, b = rows[1].name;
				fill_rgba(buf, pitch, w, h, lx, ly + 6, 14, 4, hud_rgba(rows[0].r, rows[0].g, rows[0].b, 255));
				hud_text(buf, pitch, w, h, lx + 20, ly, a.c_str(), rows[0].r, rows[0].g, rows[0].b);
				lx += 20 + ((int)a.size() + 1) * cw;
				hud_text(buf, pitch, w, h, lx, ly, "minus", 150, 160, 180);
				lx += 6 * cw;
				fill_rgba(buf, pitch, w, h, lx, ly + 6, 14, 4, hud_rgba(rows[1].r, rows[1].g, rows[1].b, 255));
				hud_text(buf, pitch, w, h, lx + 20, ly, b.c_str(), rows[1].r, rows[1].g, rows[1].b);
				lx += 20 + ((int)b.size() + 3) * cw;
			} else {
				for (auto& row : rows) {
					fill_rgba(buf, pitch, w, h, lx, ly + 6, 14, 4, hud_rgba(row.r, row.g, row.b, 255));
					hud_text(buf, pitch, w, h, lx + 20, ly, row.name.c_str(), row.r, row.g, row.b);
					lx += 20 + ((int)row.name.size() + 3) * cw;
				}
			}
			if (graph_tab == 0) hud_text(buf, pitch, w, h, lx, ly, "solid: minerals/min  faint: gas/min", 150, 160, 180);
			char mode[96];
			std::snprintf(mode, sizeof(mode), "r: %s  d: %s  f: %s   %s", graph_value_mode ? "resources" : "supply",
				graph_derivative ? "per minute" : "total", difference ? "difference" : "players", range);
			hud_text(buf, pitch, w, h, w - pad - (int)std::strlen(mode) * cw, ly, mode, 120, 130, 150);
		}

		// Plot area
		int px0 = pad + 6 * cw, py0 = ly + ch + (mini ? 6 : 10);
		int px1 = w - pad, py1 = h - pad - ch - 4;
		int end_frame = std::max(1, replay_st.end_frame);
		// Visible frame range: whole game, or the last win_s seconds ending at the current frame.
		int x_from = 0, x_to = end_frame;
		if (win_s) {
			int span = win_s * 1000 / 42;
			x_to = std::max(span, std::min(st.current_frame, end_frame));
			x_from = x_to - span;
		}
		size_t n = std::min(graph_samples.size(), (size_t)x_to / graph_sample_interval + 1);
		size_t i_from = (size_t)x_from / graph_sample_interval;
		auto series = graph_series_list(rows);
		double vmax = 0, vmin = 0;
		for (size_t i = i_from; i < n; ++i) {
			if (!graph_samples[i].valid) continue;
			for (auto& s : series) {
				double v = graph_series_value(s, i);
				vmax = std::max(vmax, v);
				vmin = std::min(vmin, v);
			}
		}
		// One round step for the whole axis, ends snapped to it.
		int y_intervals = mini ? 3 : 4;
		double ystep = nice_step(std::max(vmax - vmin, (double)y_intervals) / y_intervals);
		vmax = std::max(ystep, std::ceil(vmax / ystep) * ystep);
		vmin = std::floor(vmin / ystep) * ystep;
		auto to_x = [&](int frame) { return px0 + (int)((int64_t)(px1 - px0) * (frame - x_from) / std::max(1, x_to - x_from)); };
		auto to_y = [&](double v) { return py1 - (int)((py1 - py0) * ((v - vmin) / (vmax - vmin))); };

		// Grid and labels
		for (double v = vmin; v <= vmax + ystep * 0.01; v += ystep) {
			int y = to_y(v);
			fill_rgba(buf, pitch, w, h, px0, y, px1 - px0, 1, hud_rgba(60, 66, 80, 255));
			char label[16];
			std::snprintf(label, sizeof(label), "%d", (int)std::lround(v));
			hud_text(buf, pitch, w, h, px0 - 6 - (int)std::strlen(label) * cw, y - ch / 2, label, 150, 160, 180);
		}
		// Time ticks: the smallest step giving at most max_ticks across the visible range.
		{
			int max_ticks = mini ? 4 : 10;
			int range_s = (x_to - x_from) * 42 / 1000;
			int step = 1200;
			for (int c : {5, 10, 15, 30, 60, 120, 300, 600, 1200}) if (range_s / c <= max_ticks) { step = c; break; }
			int first = (x_from * 42 / 1000 + step - 1) / step * step;
			for (int t = first; t * 1000 / 42 <= x_to; t += step) {
				int x = to_x(t * 1000 / 42);
				fill_rgba(buf, pitch, w, h, x, py0, 1, py1 - py0, hud_rgba(40, 44, 56, 255));
				char label[16];
				std::snprintf(label, sizeof(label), "%d:%02d", t / 60, t % 60);
				int lw = (int)std::strlen(label) * cw;
				hud_text(buf, pitch, w, h, std::max(0, std::min(w - lw, x - lw / 2)), py1 + 4, label, 150, 160, 180);
			}
		}

		// Zero line: in difference mode it is the line between the players, so make it bold.
		if (difference) fill_rgba(buf, pitch, w, h, px0, to_y(0) - 1, px1 - px0, 3, hud_rgba(225, 230, 240, 255));
		else if (vmin < 0) fill_rgba(buf, pitch, w, h, px0, to_y(0), px1 - px0, 1, hud_rgba(140, 150, 170, 255));

		// Series. A segment crossing zero is split there so each part takes its sign's colour.
		// The minimized panel has no legend, so gas is dotted there to tell it apart.
		int y_zero = to_y(0);
		for (auto& s : series) {
			dash_pattern dots{2, 3};
			dash_pattern* dash = mini && s.gas ? &dots : nullptr;
			bool have_prev = false;
			int prev_x = 0, prev_y = 0;
			double prev_v = 0;
			for (size_t i = i_from; i < n; ++i) {
				if (!graph_samples[i].valid) {
					have_prev = false;
					dots.joined = false;
					continue;
				}
				double v = graph_series_value(s, i);
				int x = to_x((int)i * graph_sample_interval);
				int y = to_y(v);
				if (have_prev) {
					if ((prev_v < 0) != (v < 0)) {
						int xm = prev_x + (int)std::lround((x - prev_x) * (prev_v / (prev_v - v)));
						graph_line(buf, pitch, w, h, prev_x, prev_y, xm, y_zero, prev_v < 0 ? s.px_neg : s.px_pos, 2, dash);
						graph_line(buf, pitch, w, h, xm, y_zero, x, y, v < 0 ? s.px_neg : s.px_pos, 2, dash);
					} else {
						graph_line(buf, pitch, w, h, prev_x, prev_y, x, y, v < 0 ? s.px_neg : s.px_pos, 2, dash);
					}
				}
				prev_x = x; prev_y = y; prev_v = v; have_prev = true;
			}
		}

		// Current position
		int cx = to_x(std::min(st.current_frame, end_frame));
		fill_rgba(buf, pitch, w, h, cx, py0, 1, py1 - py0, hud_rgba(255, 255, 255, 160));
		if (!mini) {
			a_string now = format_game_time(st.current_frame);
			hud_text(buf, pitch, w, h, std::min(cx + 4, px1 - (int)now.size() * cw), py0, now.c_str(), 255, 255, 255);
		}

		graph_surface->unlock();
		int gx, gy;
		if (mini) {
			gx = viewport_x + viewport_w - w - 8;
			gy = 8;
		} else {
			gx = viewport_x + (viewport_w - w) / 2;
			gy = std::max(8, (window_height - h) / 2 - 40);
		}
		graph_area_window = rect{{gx, gy}, {gx + w, gy + h}};
		graph_surface->blit(&*window_surface, gx, gy);
	}

	// Window-pixel click on the panel: switches tab, or expands the minimized panel;
	// returns true if the click was inside.
	bool graph_panel_click(int wx, int wy) {
		if (!graphs_visible()) return false;
		auto& a = graph_area_window;
		if (wx < a.from.x || wx >= a.to.x || wy < a.from.y || wy >= a.to.y) return false;
		if (graph_mini()) {
			graph_expanded = true;
			return true;
		}
		int x = wx - a.from.x, y = wy - a.from.y;
		for (int t = 0; t != graph_tab_count; ++t) {
			auto& r = graph_tab_rects[t];
			if (x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y) graph_tab = t;
		}
		return true;
	}

	// ---- Settings menu ('?'): a checkbox for every toggle, saved between sessions ----
	// Click a row, or press its key as usual. A level setting (scroll speed) is a row of
	// notches instead: click a notch to set it, or the row's label to step up (wrapping). Choices are saved to
	// $XDG_CONFIG_HOME/openbw-viewer/settings (~/.config/... by default; %APPDATA% on
	// Windows) whenever one changes, and loaded at startup.
	bool show_settings_menu = false;
	std::unique_ptr<native_window_drawing::surface> settings_surface;
	rect settings_area_window;
	a_vector<rect> settings_row_rects;  // panel-relative, one per setting

	struct viewer_setting {
		const char* id;     // name in the settings file
		const char* label;
		const char* key;
		bool persist;
		std::function<bool()> get;
		std::function<void()> toggle;
		int levels = 0;  // > 0: a level 1..levels shown as notches, instead of a checkbox
		std::function<int()> level = {};
		std::function<void(int)> set_level = {};
	};

	void toggle_mute() {
		if (global_volume) {
			unmuted_volume = global_volume;
			set_volume(0);
		} else set_volume(unmuted_volume);
	}

	a_vector<viewer_setting> viewer_settings() {
		auto flag = [](bool& b) { return std::make_pair(std::function<bool()>([&b] { return b; }), std::function<void()>([&b] { b = !b; })); };
		a_vector<viewer_setting> r;
		auto add = [&](const char* id, const char* label, const char* key, bool persist, std::pair<std::function<bool()>, std::function<void()>> f) {
			r.push_back({id, label, key, persist, f.first, f.second});
		};
		add("hud", "Stats overlay", "", true, flag(show_hud));  // menu only, no hotkey
		add("selection_panel", "Selection panel", "i", true, flag(show_selection_panel));
		add("health_bars", "Health bars under all units", "b", true, flag(show_all_health_bars));
		add("fog", "Fog of war", "f", true, flag(show_fog));
#ifdef OPENBW_REMASTERED
		add("remastered", "Remastered graphics", "F2", true, flag(use_remastered));
#endif
		add("two_colour", "Two-colour mode (red / teal)", "shift tab", true, flag(two_colour_mode));
		add("auto_observer", "Auto observer", "o", true, std::make_pair(std::function<bool()>([this] { return auto_observer; }),
			std::function<void()>([this] { toggle_auto_observer(); })));
		add("muted", "Mute sound", "m", true, std::make_pair(std::function<bool()>([this] { return global_volume == 0; }),
			std::function<void()>([this] { toggle_mute(); })));
		{
			viewer_setting v{"scroll_speed", "Scroll speed", "", true, [] { return false; },
				[this] { scroll_speed_level = scroll_speed_level % scroll_speed_levels + 1; }};
			v.levels = scroll_speed_levels;
			v.level = [this] { return scroll_speed_level; };
			v.set_level = [this](int l) { scroll_speed_level = std::max(1, std::min((int)scroll_speed_levels, l)); };
			r.push_back(std::move(v));
		}
		add("graphs", "Graphs", "g", false, flag(show_graphs));
		add("graph_minimized", "Graphs: minimized in the corner", "m (graphs open)", true, flag(graph_minimized));
		add("graph_value", "Graphs: resource value, not supply", "r (graphs open)", true, flag(graph_value_mode));
		add("graph_derivative", "Graphs: rate of change", "d (graphs open)", true, flag(graph_derivative));
		add("graph_difference", "Graphs: players' difference", "f (graphs open)", true, flag(graph_difference));
		return r;
	}

	void toggle_auto_observer() {
		auto_observer = !auto_observer;
		observer_has_target = false;
		observer_visits.clear();
		observer_last_frame = -1;
		observer_manual_until = {};
	}

	static a_string settings_file_path() {
#ifdef EMSCRIPTEN
		return {};
#else
#ifdef _WIN32
		const char* base = std::getenv("APPDATA");
		if (!base || !*base) return {};
		return a_string(base) + "\\openbw-viewer\\settings";
#else
		const char* xdg = std::getenv("XDG_CONFIG_HOME");
		const char* home = std::getenv("HOME");
		a_string base = xdg && *xdg ? a_string(xdg) : home && *home ? a_string(home) + "/.config" : a_string();
		if (base.empty()) return {};
		return base + "/openbw-viewer/settings";
#endif
#endif
	}

	// Fingerprint of the persisted settings, to save only when one changes.
	a_string settings_saved_state;
	// Only a viewer that loaded the settings saves them, so tools that embed the UI
	// (frame dumps, evaluations) never overwrite the user's file with their defaults.
	bool settings_loaded = false;

	a_string settings_state() {
		a_string s;
		for (auto& v : viewer_settings()) {
			if (!v.persist) continue;
			s += a_string(v.id) + "=" + (v.levels ? a_string(std::to_string(v.level()).c_str()) : a_string(v.get() ? "1" : "0")) + "\n";
		}
		char z[32];
		std::snprintf(z, sizeof(z), "zoom=%g\n", zoom);
		return s + z;
	}

	void load_settings() {
		a_string path = settings_file_path();
		if (path.empty()) return;
		FILE* f = std::fopen(path.c_str(), "r");
		if (f) {
			char line[128];
			auto settings = viewer_settings();
			while (std::fgets(line, sizeof(line), f)) {
				char* eq = std::strchr(line, '=');
				if (!eq) continue;
				*eq = 0;
				if (std::strcmp(line, "zoom") == 0) {
					// Before the window exists, so set it directly; resize() applies it.
					double z = std::atof(eq + 1);
					if (z >= 0.5 && z <= 4.0) zoom = nearest_zoom_step(z);
					continue;
				}
				bool want = eq[1] == '1';
				for (auto& v : settings) {
					if (!v.persist || std::strcmp(v.id, line) != 0) continue;
					if (v.levels) v.set_level(std::atoi(eq + 1));
					else if (v.get() != want) v.toggle();
				}
			}
			std::fclose(f);
		}
		settings_saved_state = settings_state();
		settings_loaded = true;
	}

	void save_settings_if_changed() {
		if (!settings_loaded) return;
		a_string now = settings_state();
		if (now == settings_saved_state) return;
		settings_saved_state = now;
		a_string path = settings_file_path();
		if (path.empty()) return;
		// Create the directory (one level below an existing config dir); failure just means
		// the settings are not saved.
		a_string dir = path.substr(0, path.find_last_of("/\\"));
#ifdef _WIN32
		_mkdir(dir.c_str());
#elif !defined(EMSCRIPTEN)
		mkdir(dir.c_str(), 0755);
#endif
		if (FILE* f = std::fopen(path.c_str(), "w")) {
			std::fputs(now.c_str(), f);
			std::fclose(f);
		}
	}

	enum { notch_pitch = 12 };  // level notches: 9 px wide, 3 px apart

	// Panel-relative x of a level row's first notch.
	static int notches_x(int panel_w, int levels) { return panel_w - 16 - levels * notch_pitch + 3; }

	void draw_settings_menu() {
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 16, row_h = ch + 8;
		auto settings = viewer_settings();
		static const char* other_keys[] = {
			"space / p   pause            = / -   faster / slower",
			"backspace   rewind 5 s       ctrl = / - / 0   zoom",
			"wheel       zoom             arrows / edges   scroll",
			"[ ]         graph time zoom  tab, 1-5   graph tabs",
		};
		int label_cols = 0, key_w = 0, other_cols = 0;
		for (auto& v : settings) {
			label_cols = std::max(label_cols, (int)std::strlen(v.label));
			key_w = std::max(key_w, v.levels ? v.levels * notch_pitch : (int)std::strlen(v.key) * cw);
		}
		for (auto* s : other_keys) other_cols = std::max(other_cols, (int)std::strlen(s));
		int w = pad * 2 + std::max((4 + label_cols + 3) * cw + key_w, other_cols * cw);
		int h = pad * 2 + ch + 12 + (int)settings.size() * row_h + 12 + ch + 6 + (int)(sizeof(other_keys) / sizeof(*other_keys)) * ch;
		if (!settings_surface || settings_surface->w != w || settings_surface->h != h) {
			settings_surface = native_window_drawing::create_rgba_surface(w, h);
			settings_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)settings_surface->lock();
		size_t pitch = settings_surface->pitch / 4;
		fill_rgba(buf, pitch, w, h, 0, 0, w, h, hud_rgba(8, 10, 16, 230));
		bevel(buf, pitch, w, h, 0, 0, w, h, hud_rgba(150, 140, 80, 255), hud_rgba(60, 56, 30, 255));
		hud_text(buf, pitch, w, h, pad, pad, "Settings", 255, 240, 170);
		const char* close = "? / esc close";
		hud_text(buf, pitch, w, h, w - pad - (int)std::strlen(close) * cw, pad, close, 120, 130, 150);
		settings_row_rects.clear();
		int y = pad + ch + 12;
		for (auto& v : settings) {
			settings_row_rects.push_back(rect{{pad - 4, y}, {w - pad + 4, y + row_h}});
			if (v.levels) {
				// The level as a number where the checkbox goes, then the notches up to it lit.
				int lv = v.level();
				hud_text(buf, pitch, w, h, pad + 3, y + 4, std::to_string(lv).c_str(), 110, 230, 90);
				hud_text(buf, pitch, w, h, pad + 4 * cw, y + 4, v.label, 240, 240, 240);
				int nx = notches_x(w, v.levels), ny = y + (row_h - 14) / 2;
				for (int i = 0; i != v.levels; ++i) {
					int x = nx + i * notch_pitch;
					fill_rgba(buf, pitch, w, h, x, ny, notch_pitch - 3, 14, hud_rgba(20, 24, 34, 255));
					bevel(buf, pitch, w, h, x, ny, notch_pitch - 3, 14, hud_rgba(150, 140, 80, 255), hud_rgba(60, 56, 30, 255));
					if (i < lv) fill_rgba(buf, pitch, w, h, x + 2, ny + 2, notch_pitch - 7, 10, hud_rgba(110, 230, 90, 255));
				}
				y += row_h;
				continue;
			}
			bool on = v.get();
			// Checkbox
			int bx = pad, by = y + (row_h - 14) / 2;
			fill_rgba(buf, pitch, w, h, bx, by, 14, 14, hud_rgba(20, 24, 34, 255));
			bevel(buf, pitch, w, h, bx, by, 14, 14, hud_rgba(150, 140, 80, 255), hud_rgba(60, 56, 30, 255));
			if (on) fill_rgba(buf, pitch, w, h, bx + 3, by + 3, 8, 8, hud_rgba(110, 230, 90, 255));
			hud_text(buf, pitch, w, h, pad + 4 * cw, y + 4, v.label, on ? 240 : 160, on ? 240 : 165, on ? 240 : 180);
			hud_text(buf, pitch, w, h, w - pad - (int)std::strlen(v.key) * cw, y + 4, v.key, 120, 130, 150);
			y += row_h;
		}
		y += 12;
		hud_text(buf, pitch, w, h, pad, y, "Other keys", 255, 240, 170);
		y += ch + 6;
		for (auto* s : other_keys) {
			hud_text(buf, pitch, w, h, pad, y, s, 150, 160, 180);
			y += ch;
		}
		settings_surface->unlock();
		int gx = viewport_x + (viewport_w - w) / 2, gy = std::max(8, (window_height - h) / 2);
		settings_area_window = rect{{gx, gy}, {gx + w, gy + h}};
		settings_surface->blit(&*window_surface, gx, gy);
	}

	// Click while the menu is open: toggles a row; any click inside is consumed.
	bool settings_menu_click(int wx, int wy) {
		if (!show_settings_menu) return false;
		auto& a = settings_area_window;
		if (wx < a.from.x || wx >= a.to.x || wy < a.from.y || wy >= a.to.y) return false;
		int x = wx - a.from.x, y = wy - a.from.y;
		auto settings = viewer_settings();
		for (size_t i = 0; i != settings_row_rects.size() && i != settings.size(); ++i) {
			auto& r = settings_row_rects[i];
			if (x < r.from.x || x >= r.to.x || y < r.from.y || y >= r.to.y) continue;
			auto& v = settings[i];
			int nx = notches_x(a.to.x - a.from.x, v.levels);
			// A click on (or between) the notches sets the level; elsewhere on the row steps it.
			if (v.levels && x >= nx - 3) v.set_level((x - (nx - 3)) / notch_pitch + 1);
			else v.toggle();
		}
		return true;
	}

	// ---- Replay bar: game timer, speed and seek bar, bottom-right, drawn crisp ----
	// Styled after Brood War's console: black panel, olive bevelled frame, hp-bar green
	// progress, pale yellow timer text.
	std::unique_ptr<native_window_drawing::surface> replay_bar_surface, replay_tip_surface;

	// Frame under the cursor on the seek bar (window pixels, same mapping as a click), or -1
	// when the cursor is elsewhere. While dragging, the cursor may stray off the bar.
	int replay_bar_hover_frame(const rect& bar) {
		if (!wnd.has_mouse_focus()) return -1;
		int mx = -1, my = -1;
		wnd.get_cursor_pos(&mx, &my);
		int slack = (int)(4 * zoom);
		bool over = mx >= bar.from.x && mx < bar.to.x && my >= bar.from.y - slack && my < bar.to.y + slack;
		if (!over && !is_moving_replay_slider) return -1;
		int bw = std::max(1, bar.to.x - bar.from.x);
		int x = std::max(0, std::min(bw, mx - bar.from.x));
		return (int)((int64_t)x * replay_st.end_frame / bw);
	}

	// 1x is OpenBW's normal replay rate (42 ms per frame, Brood War's "Fastest").
	a_string speed_label() const {
		char buf[48];
		int raw = (int)game_speed.raw_value, one = (int)fp8::integer(1).raw_value;
		if (raw >= one && raw % one) std::snprintf(buf, sizeof(buf), "%.1fx", raw / (double)one);
		else if (raw >= one) std::snprintf(buf, sizeof(buf), "%dx", raw / one);
		else std::snprintf(buf, sizeof(buf), "1/%dx", one / std::max(1, raw));
		return is_paused ? a_string("Paused ") + buf : a_string(buf);
	}

	void bevel(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int rw, int rh, uint32_t light, uint32_t dark) {
		fill_rgba(buf, pitch, w, h, x, y, rw, 1, light);
		fill_rgba(buf, pitch, w, h, x, y, 1, rh, light);
		fill_rgba(buf, pitch, w, h, x, y + rh - 1, rw, 1, dark);
		fill_rgba(buf, pitch, w, h, x + rw - 1, y, 1, rh, dark);
	}

	// ---- Replay controls: slower, pause/play and faster, left of the seek bar ----
	enum replay_button { replay_button_slower, replay_button_pause, replay_button_faster, replay_button_count };
	enum { replay_controls_w = 24 + 4 + 32 + 4 + 24 + 12 };  // buttons, then a divider

	// The replay panel in window pixels: its outer rect and each button's rect.
	struct replay_panel_layout {
		rect panel;
		std::array<rect, replay_button_count> buttons;
	};
	replay_panel_layout replay_panel() const {
		const int ch = hud_font::cell_h, pad = 8;
		rect bar = replay_bar_window_rect();
		replay_panel_layout r;
		r.panel = rect{{bar.from.x - pad - replay_controls_w, bar.from.y - pad - ch - 6}, {bar.to.x + pad, bar.to.y + pad}};
		int inner_top = r.panel.from.y + pad, inner_h = ch + 6 + (bar.to.y - bar.from.y);
		int x = r.panel.from.x + pad;
		auto button = [&](int size) {
			int y = inner_top + (inner_h - size) / 2;
			rect b{{x, y}, {x + size, y + size}};
			x += size + 4;
			return b;
		};
		r.buttons[replay_button_slower] = button(24);
		r.buttons[replay_button_pause] = button(32);
		r.buttons[replay_button_faster] = button(24);
		return r;
	}

	static bool rect_contains(const rect& r, int x, int y) { return x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y; }

	bool can_step_speed(bool faster) const {
		return faster ? game_speed < fp8::integer(128) : game_speed > fp8::from_raw(fp8::integer(1).raw_value / 4);
	}

	int replay_button_at(int wx, int wy) const {
		auto layout = replay_panel();
		for (int i = 0; i != replay_button_count; ++i) if (rect_contains(layout.buttons[i], wx, wy)) return i;
		return -1;
	}

	int replay_button_held = -1;  // pressed with the mouse, until it is released

	// Mouse down (window pixels): presses a control. Any click on the panel outside the seek
	// bar is consumed, so it doesn't select or move the camera.
	bool replay_controls_click(int wx, int wy) {
		if (replay_st.end_frame == 0 || !show_replay_panel()) return false;
		int b = replay_button_at(wx, wy);
		if (b == replay_button_pause) is_paused = !is_paused;
		else if (b == replay_button_slower || b == replay_button_faster) step_speed(b == replay_button_faster);
		if (b >= 0) {
			replay_button_held = b;
			return true;
		}
		rect bar = replay_bar_window_rect();
		int slack = (int)(4 * zoom);
		bool on_bar = wx >= bar.from.x && wx < bar.to.x && wy >= bar.from.y - slack && wy < bar.to.y + slack;
		return !on_bar && rect_contains(replay_panel().panel, wx, wy);
	}

	bool show_replay_panel() const { return draw_ui_elements; }

	// Pixel-art icons for the buttons, centred in a size x size square at (x, y).
	void draw_play_icon(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int size, uint32_t c) {
		int th = size / 2 | 1, tw = th / 2 + 2, x0 = x + (size - tw) / 2 + 1, y0 = y + (size - th) / 2;
		for (int i = 0; i != tw; ++i) {
			int half = (th / 2) * (tw - i) / tw;
			fill_rgba(buf, pitch, w, h, x0 + i, y0 + th / 2 - half, 1, half * 2 + 1, c);
		}
	}
	void draw_pause_icon(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int size, uint32_t c) {
		int bh = size / 2, bw = std::max(3, size / 8), gap = bw;
		int x0 = x + (size - bw * 2 - gap) / 2, y0 = y + (size - bh) / 2;
		fill_rgba(buf, pitch, w, h, x0, y0, bw, bh, c);
		fill_rgba(buf, pitch, w, h, x0 + bw + gap, y0, bw, bh, c);
	}
	// Two chevron-triangles pointing right (faster) or left (slower).
	void draw_speed_icon(uint32_t* buf, size_t pitch, int w, int h, int x, int y, int size, bool right, uint32_t c) {
		int th = size / 2 - 1 | 1, tw = th / 2 + 1, total = tw * 2, x0 = x + (size - total) / 2, y0 = y + (size - th) / 2;
		for (int t = 0; t != 2; ++t) {
			for (int i = 0; i != tw; ++i) {
				int col = right ? i : tw - 1 - i;
				int half = (th / 2) * (tw - i) / tw;
				fill_rgba(buf, pitch, w, h, x0 + t * tw + col, y0 + th / 2 - half, 1, half * 2 + 1, c);
			}
		}
	}

	// A console-style button: bevelled olive frame, sunk while pressed, lit while hovered.
	void draw_replay_button(uint32_t* buf, size_t pitch, int w, int h, const rect& r, bool hover, bool pressed, bool enabled) {
		int x = r.from.x, y = r.from.y, bw = r.to.x - r.from.x, bh = r.to.y - r.from.y;
		fill_rgba(buf, pitch, w, h, x, y, bw, bh, hud_rgba(0, 0, 0, 255));
		uint32_t face = pressed ? hud_rgba(14, 18, 10, 255) : hover && enabled ? hud_rgba(52, 62, 36, 255) : hud_rgba(30, 36, 22, 255);
		fill_rgba(buf, pitch, w, h, x + 1, y + 1, bw - 2, bh - 2, face);
		// Lighter upper half, as on the console's buttons.
		if (!pressed) fill_rgba(buf, pitch, w, h, x + 2, y + 2, bw - 4, (bh - 4) / 2, hover && enabled ? hud_rgba(62, 74, 44, 255) : hud_rgba(38, 46, 28, 255));
		uint32_t light = hud_rgba(150, 164, 108, 255), dark = hud_rgba(44, 50, 32, 255);
		if (pressed) std::swap(light, dark);
		bevel(buf, pitch, w, h, x + 1, y + 1, bw - 2, bh - 2, light, dark);
	}

	void draw_replay_controls(uint32_t* buf, size_t pitch, int w, int h, int ox, int oy) {
		auto layout = replay_panel();
		int mx = -1, my = -1;
		if (wnd.has_mouse_focus()) wnd.get_cursor_pos(&mx, &my);
		int hovered = replay_button_at(mx, my);
		for (int i = 0; i != replay_button_count; ++i) {
			rect r = layout.buttons[i];
			bool enabled = i == replay_button_pause || can_step_speed(i == replay_button_faster);
			bool pressed = replay_button_held == i && hovered == i;
			draw_replay_button(buf, pitch, w, h, rect{{r.from.x - ox, r.from.y - oy}, {r.to.x - ox, r.to.y - oy}}, hovered == i, pressed, enabled);
			int size = r.to.x - r.from.x, x = r.from.x - ox + (pressed ? 1 : 0), y = r.from.y - oy + (pressed ? 1 : 0);
			uint32_t c = !enabled ? hud_rgba(80, 86, 64, 255) : hovered == i ? hud_rgba(255, 252, 220, 255) : hud_rgba(248, 240, 170, 255);
			if (i == replay_button_pause) {
				if (is_paused) draw_play_icon(buf, pitch, w, h, x, y, size, hovered == i ? hud_rgba(190, 255, 160, 255) : hud_rgba(150, 230, 110, 255));
				else draw_pause_icon(buf, pitch, w, h, x, y, size, c);
			} else draw_speed_icon(buf, pitch, w, h, x, y, size, i == replay_button_faster, c);
		}
		// Divider between the controls and the timer/seek bar.
		int dx = layout.buttons[replay_button_faster].to.x - ox + 6;
		fill_rgba(buf, pitch, w, h, dx, 6, 1, h - 12, hud_rgba(24, 28, 18, 255));
		fill_rgba(buf, pitch, w, h, dx + 1, 6, 1, h - 12, hud_rgba(84, 94, 60, 255));
	}

	const char* replay_button_tip(int b) const {
		if (b == replay_button_slower) return "Slower  (-)";
		if (b == replay_button_faster) return "Faster  (=)";
		return is_paused ? "Play  (space)" : "Pause  (space)";
	}

	void draw_replay_bar() {
		if (replay_st.end_frame == 0) return;
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 8;
		rect bar = replay_bar_window_rect();
		int bw = bar.to.x - bar.from.x, bh = bar.to.y - bar.from.y;
		auto layout = replay_panel();
		int w = layout.panel.to.x - layout.panel.from.x, h = layout.panel.to.y - layout.panel.from.y;
		int ox = layout.panel.from.x, oy = layout.panel.from.y;
		const int lx = bar.from.x - ox;  // left edge of the timer and seek bar in the panel
		if (!replay_bar_surface || replay_bar_surface->w != w || replay_bar_surface->h != h) {
			replay_bar_surface = native_window_drawing::create_rgba_surface(w, h);
			replay_bar_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)replay_bar_surface->lock();
		size_t pitch = replay_bar_surface->pitch / 4;
		fill_rgba(buf, pitch, w, h, 0, 0, w, h, hud_rgba(4, 6, 4, 215));
		bevel(buf, pitch, w, h, 0, 0, w, h, hud_rgba(120, 132, 88, 255), hud_rgba(44, 50, 32, 255));
		bevel(buf, pitch, w, h, 1, 1, w - 2, h - 2, hud_rgba(64, 72, 46, 255), hud_rgba(24, 28, 18, 255));

		a_string timer = format_game_time(st.current_frame) + " / " + format_game_time(replay_st.end_frame);
		hud_text(buf, pitch, w, h, lx, pad - 2, timer.c_str(), 248, 240, 170);
		if (auto_observer) {
			bool held = clock.now() < observer_manual_until;
			hud_text(buf, pitch, w, h, lx + ((int)timer.size() + 2) * cw, pad - 2, held ? "AUTO (manual)" : "AUTO", held ? 150 : 120, held ? 150 : 220, held ? 150 : 255);
		}
		a_string speed = speed_label();
		if (is_paused) hud_text(buf, pitch, w, h, w - pad - (int)speed.size() * cw, pad - 2, speed.c_str(), 255, 190, 70);
		else hud_text(buf, pitch, w, h, w - pad - (int)speed.size() * cw, pad - 2, speed.c_str(), 150, 230, 110);

		// Track: segmented like the in-game bars
		int tx = lx, ty = pad + ch + 4;
		draw_bw_bar(buf, pitch, w, h, tx, ty, bw, bh, (double)std::min(st.current_frame, replay_st.end_frame) / replay_st.end_frame, 3);
		auto frame_x = [&](int frame) { return tx + 2 + (int)((int64_t)(bw - 4) * std::max(0, std::min(frame, replay_st.end_frame)) / replay_st.end_frame); };
		int fx = frame_x(st.current_frame);
		if (replay_frame != st.current_frame) {  // seek target while catching up
			int sx = frame_x(replay_frame);
			fill_rgba(buf, pitch, w, h, sx, ty - 2, 1, bh + 4, hud_rgba(248, 240, 170, 255));
		}
		int hover = replay_bar_hover_frame(bar);
		if (hover >= 0) fill_rgba(buf, pitch, w, h, frame_x(hover), ty, 1, bh, hud_rgba(248, 240, 170, 200));
		// Thumb
		fill_rgba(buf, pitch, w, h, fx - 2, ty - 3, 5, bh + 6, hud_rgba(20, 24, 16, 255));
		fill_rgba(buf, pitch, w, h, fx - 1, ty - 2, 3, bh + 4, hud_rgba(236, 244, 214, 255));
		draw_replay_controls(buf, pitch, w, h, ox, oy);

		replay_bar_surface->unlock();
		replay_bar_surface->blit(&*window_surface, ox, oy);
		if (hover >= 0) draw_replay_tip(format_game_time(hover), ox + frame_x(hover), oy);
		else if (wnd.has_mouse_focus()) {
			int mx = -1, my = -1;
			wnd.get_cursor_pos(&mx, &my);
			int b = replay_button_at(mx, my);
			if (b >= 0) draw_replay_tip(replay_button_tip(b), (layout.buttons[b].from.x + layout.buttons[b].to.x) / 2, oy);
		}
	}

	// Small timer label centred on x, just above the replay bar panel whose top is panel_y.
	void draw_replay_tip(const a_string& text, int x, int panel_y) {
		const int cw = hud_font::cell_w, ch = hud_font::cell_h;
		int w = (int)text.size() * cw + 10, h = ch + 6;
		if (!replay_tip_surface || replay_tip_surface->w != w || replay_tip_surface->h != h) {
			replay_tip_surface = native_window_drawing::create_rgba_surface(w, h);
			replay_tip_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)replay_tip_surface->lock();
		size_t pitch = replay_tip_surface->pitch / 4;
		fill_rgba(buf, pitch, w, h, 0, 0, w, h, hud_rgba(4, 6, 4, 230));
		bevel(buf, pitch, w, h, 0, 0, w, h, hud_rgba(120, 132, 88, 255), hud_rgba(44, 50, 32, 255));
		hud_text(buf, pitch, w, h, 5, 2, text.c_str(), 248, 240, 170);
		replay_tip_surface->unlock();
		int left = std::max(viewport_x, std::min(viewport_x + viewport_w - w, x - w / 2));
		replay_tip_surface->blit(&*window_surface, left, std::max(0, panel_y - h - 3));
	}

	a_vector<unit_id> current_selection;

	bool current_selection_is_selected(unit_t* u) {
		auto uid = get_unit_id(u);
		return std::find(current_selection.begin(), current_selection.end(), uid) != current_selection.end();
	}

	void current_selection_add(unit_t* u) {
		auto uid = get_unit_id(u);
		if (std::find(current_selection.begin(), current_selection.end(), uid) != current_selection.end()) return;
		current_selection.push_back(uid);
	}

	void current_selection_clear() {
		current_selection.clear();
	}

	void current_selection_remove(const unit_t* u) {
		auto uid = get_unit_id(u);
		auto i = std::find(current_selection.begin(), current_selection.end(), uid);
		if (i != current_selection.end()) current_selection.erase(i);
	}

	bool is_moving_minimap = false;
	bool is_moving_replay_slider = false;
	bool is_paused = false;
	bool is_drag_selecting = false;
	bool is_dragging_screen = false;
	int drag_select_from_x = 0;
	int drag_select_from_y = 0;
	int drag_select_to_x = 0;
	int drag_select_to_y = 0;
	xy drag_screen_pos;

	// ---- Mouse cursor ----
	// Brood War's arrow, animated (a glint runs round its outline): Remastered's art with the
	// remastered graphics on, else the classic cursor/arrow.grp in the game palette, at the
	// selection panel's scale. The hotspot is the arrow's tip.
	int cursor_key = -1;  // what the window's cursor was built for (see update_cursor)
	size_t cursor_frame_count = 0;
	std::chrono::high_resolution_clock::time_point cursor_start;

	// Calls f(x, y, palette index) for each opaque pixel of a classic grp frame.
	template <typename F>
	static void for_each_grp_pixel(const grp_t::frame_t& f, F&& fn) {
		for (size_t y = 0; y != f.size.y; ++y) {
			const uint8_t* p = f.data_container.data() + f.line_data_offset.at(y);
			for (size_t x = 0; x < f.size.x;) {
				uint8_t c = *p++;
				if (c & 0x80) {
					x += c & 0x7f;
				} else if (c & 0x40) {
					uint8_t v = *p++;
					for (int i = 0; i != (c & 0x3f) && x < f.size.x; ++i, ++x) fn(x, y, v);
				} else {
					for (int i = 0; i != c && x < f.size.x; ++i, ++x) fn(x, y, *p++);
				}
			}
		}
	}

	// The classic arrow's frames, `scale` x enlarged and coloured by `palette` (256 RGBx
	// entries). A Brood War cursor's hotspot is the centre of its grp.
	static std::vector<native_window::cursor_image> classic_cursor_frames(const grp_t& grp, const uint8_t* palette, int scale) {
		std::vector<native_window::cursor_image> out;
		for (auto& f : grp.frames) {
			native_window::cursor_image c;
			c.w = (int)f.size.x * scale;
			c.h = (int)f.size.y * scale;
			if (c.w <= 0 || c.h <= 0) continue;
			c.hot_x = std::max(0, std::min(c.w - 1, ((int)grp.width / 2 - (int)f.offset.x) * scale));
			c.hot_y = std::max(0, std::min(c.h - 1, ((int)grp.height / 2 - (int)f.offset.y) * scale));
			c.px.assign((size_t)c.w * c.h, 0);
			for_each_grp_pixel(f, [&](size_t x, size_t y, uint8_t v) {
				uint32_t rgb = palette[4 * v] | palette[4 * v + 1] << 8 | palette[4 * v + 2] << 16 | 0xffu << 24;
				for (int Y = (int)y * scale; Y != ((int)y + 1) * scale; ++Y) {
					std::fill_n(c.px.begin() + (size_t)Y * c.w + x * scale, scale, rgb);
				}
			});
			out.push_back(std::move(c));
		}
		return out;
	}

#ifdef OPENBW_REMASTERED
	// Remastered's arrow (the HD2 set, which is twice the classic size) at the size of
	// `classic`'s frames, with their hotspots; empty without the art or on a frame mismatch.
	std::vector<native_window::cursor_image> hd_cursor_frames(const std::vector<native_window::cursor_image>& classic) {
		std::vector<native_window::cursor_image> out;
		if (!hd || classic.empty()) return out;
		auto frames = hd->read_frames("HD2/cursor/arrow.dds.grp");
		if (frames.size() != classic.size()) return out;
		for (size_t i = 0; i != frames.size(); ++i) {
			auto& src = frames[i];
			native_window::cursor_image c = classic[i];
			if (src.w <= 0 || src.h <= 0) return {};
			if (src.w != c.w || src.h != c.h) {
				scr::premultiply(src);
				src = scr::resample(src, 0, 0, src.w, src.h, c.w, c.h, true);
				scr::unpremultiply(src);
			}
			c.px = std::move(src.px);
			out.push_back(std::move(c));
		}
		return out;
	}
#endif

	// The cursor frames for the current settings (remastered on/off, panel scale).
	std::vector<native_window::cursor_image> cursor_frames(bool remastered, int scale) {
		std::vector<native_window::cursor_image> classic;
		if (tileset_img.wpe.size() < 256 * 4) return classic;
		try {
			a_vector<uint8_t> data;
			load_data_file(data, "cursor/arrow.grp");
			auto grp = read_grp(data_loading::data_reader_le(data.data(), data.data() + data.size()));
			classic = classic_cursor_frames(grp, tileset_img.wpe.data(), scale);
		} catch (const std::exception& e) {
			ui::log("cursor: %s\n", e.what());
			return {};
		}
#ifdef OPENBW_REMASTERED
		if (remastered) {
			auto frames = hd_cursor_frames(classic);
			if (!frames.empty()) return frames;
		}
#else
		(void)remastered;
#endif
		return classic;
	}

	// Rebuilds the window's cursor when the graphics or scale change, and animates it.
	void update_cursor(std::chrono::high_resolution_clock::time_point now) {
		if (!wnd || tileset_img.wpe.size() < 256 * 4) return;
		const int scale = window_height >= 1000 ? 2 : 1;
		bool remastered = false;
#ifdef OPENBW_REMASTERED
		remastered = use_remastered && hd;
#endif
		const int key = scale * 2 + remastered;
		if (key != cursor_key) {
			cursor_key = key;
			cursor_frame_count = wnd.set_cursor_frames(cursor_frames(remastered, scale));
			cursor_start = now;
		}
		if (cursor_frame_count > 1) {
			const auto frame_time = std::chrono::milliseconds(100);
			wnd.show_cursor_frame((size_t)((now - cursor_start) / frame_time) % cursor_frame_count);
		}
	}

	void update() {
		auto now = clock.now();
		update_cursor(now);

		if (now - last_fps >= std::chrono::seconds(1)) {
			//ui::log("draw fps: %g\n", fps_counter / std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1, 1>>>(now - last_fps).count());
			last_fps = now;
			fps_counter = 0;
			if (wnd) refresh_hz = wnd.refresh_rate();  // the window may have moved to another display
		}
		++fps_counter;

		auto minimap_area = get_minimap_area();
		auto replay_slider_area = get_replay_slider_area();

		auto move_minimap = [&](int mouse_x, int mouse_y) {
			if (mouse_x < minimap_area.from.x) mouse_x = minimap_area.from.x;
			else if (mouse_x >= minimap_area.to.x) mouse_x = minimap_area.to.x - 1;
			if (mouse_y < minimap_area.from.y) mouse_y = minimap_area.from.y;
			else if (mouse_y >= minimap_area.to.y) mouse_y = minimap_area.to.y - 1;
			int x = mouse_x - minimap_area.from.x;
			int y = mouse_y - minimap_area.from.y;
			x = x * game_st.map_tile_width / (minimap_area.to.x - minimap_area.from.x);
			y = y * game_st.map_tile_height / (minimap_area.to.y - minimap_area.from.y);
			screen_pos = xy(32 * x - view_width / 2, 32 * y - view_height / 2);
		};

		auto check_move_minimap = [&](auto& e) {
			if (e.mouse_x >= minimap_area.from.x && e.mouse_x < minimap_area.to.x) {
				if (e.mouse_y >= minimap_area.from.y && e.mouse_y < minimap_area.to.y) {
					is_moving_minimap = true;
					move_minimap(e.mouse_x, e.mouse_y);
				}
			}
		};

		auto move_replay_slider = [&](int mouse_x, int mouse_y) {
			(void)mouse_y;
			int ow = std::max(1, replay_slider_area.to.x - replay_slider_area.from.x);
			int x = std::max(0, std::min(ow, mouse_x - replay_slider_area.from.x));
			replay_frame = (int)((int64_t)x * replay_st.end_frame / ow);
		};

		auto check_move_replay_slider = [&](auto& e) {
			if (e.mouse_x >= replay_slider_area.from.x && e.mouse_x < replay_slider_area.to.x) {
				if (e.mouse_y >= replay_slider_area.from.y - 4 && e.mouse_y < replay_slider_area.to.y + 4) {
					is_moving_replay_slider = true;
					move_replay_slider(e.mouse_x, e.mouse_y);
				}
			}
		};

		auto end_drag_select = [&](bool double_clicked) {
			bool shift = wnd.get_key_state(225) || wnd.get_key_state(229);
			if (drag_select_from_x > drag_select_to_x) std::swap(drag_select_from_x, drag_select_to_x);
			if (drag_select_from_y > drag_select_to_y) std::swap(drag_select_from_y, drag_select_to_y);
			if (drag_select_to_x - drag_select_from_x <= 4 || drag_select_to_y - drag_select_from_y <= 4) {
				unit_t* u = select_get_unit_at(screen_pos + xy(drag_select_from_x, drag_select_from_y));
				if (u) {
					bool ctrl = wnd.get_key_state(224) || wnd.get_key_state(228);
					if (double_clicked || ctrl) {
						if (!shift) current_selection_clear();
						auto is_tank = [&](unit_t* a) {
							return unit_is(a, UnitTypes::Terran_Siege_Tank_Siege_Mode) || unit_is(a, UnitTypes::Terran_Siege_Tank_Tank_Mode);
						};
						auto is_same_type = [&](unit_t* a, unit_t* b) {
							if (unit_is_mineral_field(a) && unit_is_mineral_field(b)) return true;
							if (is_tank(a) && is_tank(b)) return true;
							return a->unit_type == b->unit_type;
						};
						for (unit_t* u2 : find_units({screen_pos, screen_pos + xy(view_width, view_height)})) {
							if (u2->owner != u->owner) continue;
							if (!is_same_type(u, u2)) continue;
							current_selection_add(u2);
						}
					} else {
						if (shift) {
							if (current_selection_is_selected(u)) current_selection_remove(u);
							else current_selection_add(u);
						} else {
							current_selection_clear();
							current_selection_add(u);
						}
					}
				}
			} else {
				if (!shift) current_selection_clear();
				auto r = rect{{drag_select_from_x, drag_select_from_y}, {drag_select_to_x, drag_select_to_y}};
				if (r.from.x > r.to.x) std::swap(r.from.x, r.to.x);
				if (r.from.y > r.to.y) std::swap(r.from.y, r.to.y);
				a_vector<unit_t*> new_units;
				bool any_non_neutrals = false;
				bool any_non_buildings = false;
				for (unit_t* u : find_units(translate_rect(r, screen_pos))) {
					if (!unit_can_be_selected(u)) continue;
					new_units.push_back(u);
					if (u->owner != 11) any_non_neutrals = true;
					if (!ut_building(u)) any_non_buildings = true;
				}
				// As in Brood War, a box that catches units leaves buildings out.
				for (unit_t* u : new_units) {
					if (u->owner == 11 && any_non_neutrals) continue;
					if (ut_building(u) && any_non_buildings) continue;
					current_selection_add(u);
				}
			}
			is_drag_selecting = false;
		};

		xy camera_before_input = screen_pos;
		if (wnd) {
			native_window::event_t e;
			while (wnd.peek_message(e)) {
				if (e.type == native_window::event_t::type_mouse_wheel && graphs_visible() && graph_area_window.from.x <= e.mouse_x && e.mouse_x < graph_area_window.to.x && graph_area_window.from.y <= e.mouse_y && e.mouse_y < graph_area_window.to.y) {
					step_graph_window(e.wheel_y > 0 ? +1 : -1);
					continue;
				}
				if (e.type == native_window::event_t::type_mouse_wheel) {
					// mouse_x/y are window pixels here (the anchor), not logical ones.
					step_zoom(e.wheel_y > 0 ? +1 : -1, std::max(0, std::min(viewport_w - 1, e.mouse_x - viewport_x)), e.mouse_y);
					continue;
				}
				if (e.type == native_window::event_t::type_mouse_button_down && settings_menu_click(e.mouse_x, e.mouse_y)) continue;
				if (e.type == native_window::event_t::type_mouse_button_down && e.button == 1 && replay_controls_click(e.mouse_x, e.mouse_y)) continue;
				if (e.type == native_window::event_t::type_mouse_button_up && e.button == 1 && replay_button_held >= 0) {
					replay_button_held = -1;
					continue;
				}
				if (e.type == native_window::event_t::type_mouse_button_down && graph_panel_click(e.mouse_x, e.mouse_y)) continue;
				if (e.type == native_window::event_t::type_mouse_button_down && e.button == 1) {
					bool shift = wnd.get_key_state(225) || wnd.get_key_state(229);
					bool ctrl = wnd.get_key_state(224) || wnd.get_key_state(228);
					if (selection_panel_click(e.mouse_x, e.mouse_y, shift, ctrl)) continue;
				}
				if (zoom != 1.0 || viewport_x != 0) {
					e.mouse_x = (int)((e.mouse_x - viewport_x) / zoom);
					e.mouse_y = (int)(e.mouse_y / zoom);
				}
				switch (e.type) {
				case native_window::event_t::type_quit:
					if (exit_on_close) std::exit(0);
					else window_closed = true;
					break;
				case native_window::event_t::type_resize:
					resize(e.width, e.height);
					break;
				case native_window::event_t::type_mouse_button_down:
					if (e.button == 1) {
						check_move_minimap(e);
						check_move_replay_slider(e);
						if (!is_moving_minimap && !is_moving_replay_slider) {
							is_drag_selecting = true;
							drag_select_from_x = e.mouse_x;
							drag_select_from_y = e.mouse_y;
							drag_select_to_x = e.mouse_x;
							drag_select_to_y = e.mouse_y;
						}
					} else if (e.button == 3) {
						is_dragging_screen = true;
						drag_screen_pos = screen_pos + xy((fp16::integer(e.mouse_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y) / view_scale).integer_part());
					}
					break;
				case native_window::event_t::type_mouse_motion:
					if (e.button_state & 1) {
						if (is_moving_minimap) check_move_minimap(e);
						if (is_moving_replay_slider) check_move_replay_slider(e);
						if (is_drag_selecting) {
							drag_select_to_x = e.mouse_x;
							drag_select_to_y = e.mouse_y;
						}
					} else if (e.button_state & 4) {
						if (is_dragging_screen) {
							screen_pos = drag_screen_pos - xy((fp16::integer(e.mouse_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y) / view_scale).integer_part());
							//screen_pos -= xy((fp16::integer(e.mouse_x - drag_screen_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y - drag_screen_y) / view_scale).integer_part());
						}
					}

					if (is_drag_selecting && ~e.button_state & 1) end_drag_select(false);
					break;
				case native_window::event_t::type_mouse_button_up:
					if (e.button == 1) {
						if (is_moving_minimap) is_moving_minimap = false;
						if (is_moving_replay_slider) is_moving_replay_slider = false;
						if (is_drag_selecting) {
							end_drag_select(e.clicks >= 2 && e.clicks % 2 == 0);
						}
					} else if (e.button == 3) {
						is_dragging_screen = false;
					}
					break;
				case native_window::event_t::type_key_down:
					//if (e.sym == 'q') {
					//	use_new_images = !use_new_images;
					//}
#ifndef EMSCRIPTEN
					if (e.sym == ' ' || e.sym == 'p') {
						is_paused = !is_paused;
					}
					{
						bool ctrl = wnd.get_key_state(224) || wnd.get_key_state(228);
						// SDLK_KP_MINUS / SDLK_KP_PLUS / SDLK_KP_EQUALS
						bool minus = e.sym == '-' || e.sym == 0x40000056;
						bool plus = e.sym == '=' || e.sym == '+' || e.sym == 0x40000057 || e.sym == 0x40000067;
						if (ctrl && minus) step_zoom(-1);
						else if (ctrl && plus) step_zoom(+1);
						else if (ctrl && e.sym == '0') set_zoom(1.0);
						else if (minus) step_speed(false);
						else if (plus) step_speed(true);
					}
					if (e.sym == 'b') show_all_health_bars = !show_all_health_bars;
					if (e.sym == 0x4000003B) use_remastered = !use_remastered;  // F2
					if (e.sym == 'i') show_selection_panel = !show_selection_panel;
					if (e.sym == 'g') {
						if (graph_minimized) graph_expanded = !graph_expanded;  // maximize / minimize
						else show_graphs = !show_graphs;
					}
					if (e.sym == 27 && show_settings_menu) show_settings_menu = false;  // Esc: menu first
					else if (e.sym == 27 && graph_minimized && graph_expanded) graph_expanded = false;
					else if (e.sym == 27 && show_graphs && !graph_minimized) show_graphs = false;
					if (e.sym == '/' && (wnd.get_key_state(225) || wnd.get_key_state(229))) show_settings_menu = !show_settings_menu;  // '?'
					if (e.sym == '?') show_settings_menu = !show_settings_menu;  // layouts that report '?' itself
					// Graph keys act only while the full panel shows; on the minimized panel d, f and
					// m keep their other meanings (slow down, fog, mute).
					if (graph_full() && e.sym == ']') step_graph_window(+1);
					if (graph_full() && e.sym == 'r') graph_value_mode = !graph_value_mode;
					if (e.sym == 'o') toggle_auto_observer();
					if (graph_full() && e.sym == '[') step_graph_window(-1);
					if (e.sym == '\t') {
						bool shift = wnd.get_key_state(225) || wnd.get_key_state(229);
						if (shift) two_colour_mode = !two_colour_mode;
						else if (graph_full()) graph_tab = (graph_tab + 1) % graph_tab_count;
					}
					if (graph_full() && e.sym >= '1' && e.sym < '1' + graph_tab_count) graph_tab = e.sym - '1';
					if (graph_arrows_switch_tabs() && (e.scancode == 79 || e.scancode == 80)) {  // right / left
						graph_tab = (graph_tab + (e.scancode == 79 ? 1 : graph_tab_count - 1)) % graph_tab_count;
					}
					if (e.sym == 'f') {
						if (graph_full()) graph_difference = !graph_difference;
						else show_fog = !show_fog;
					}
					if (graph_full() && e.sym == 'm') {
						graph_minimized = !graph_minimized;
						graph_expanded = false;
						if (!graph_minimized) show_graphs = true;  // unpinned: stay open, full size
					}
					else if (e.sym == 'm') toggle_mute();
					if (e.sym == 'a' || e.sym == 'u') step_speed(true);
					if (e.sym == 'z' || (e.sym == 'd' && !graph_full())) step_speed(false);
					if (e.sym == 'd' && graph_full()) graph_derivative = !graph_derivative;
					if (e.sym == '\b') {
						int t = 5 * 1000 / 42;  // 5 game seconds at 42 ms per frame
						if (replay_frame < t) replay_frame = 0;
						else replay_frame -= t;
					}
#endif
					break;
				}
			}
		}

		if (!indexed_surface) {
			if (wnd) {
				window_surface = native_window_drawing::get_window_surface(&wnd);
				rgba_surface = native_window_drawing::create_rgba_surface(screen_width, screen_height);
			} else {
				rgba_surface = native_window_drawing::create_rgba_surface(screen_width, screen_height);
			}
			indexed_surface = native_window_drawing::convert_to_8_bit_indexed(&*rgba_surface);
			if (!palette) set_image_data();
			indexed_surface->set_palette(palette);

			indexed_surface->set_blend_mode(native_window_drawing::blend_mode::none);
			rgba_surface->set_blend_mode(native_window_drawing::blend_mode::none);
			rgba_surface->set_alpha(0);

			if (window_surface) {
				window_surface->set_blend_mode(native_window_drawing::blend_mode::none);
				window_surface->set_alpha(0);
			}
		}

		if (wnd) {
			// Keyboard and edge scrolling: scroll_speeds[n] logical pixels per 12 ms, n rising
			// every 12 ms the scroll is held. The camera moves by the time since the last frame,
			// keeping the sub-pixel remainder, so the motion is even at any frame rate.
			const auto scroll_step = std::chrono::milliseconds(12);
			static const std::array<int, 6> scroll_speeds = {3, 3, 5, 8, 8, 10};
			auto elapsed = now - last_input_poll;
			if (elapsed > scroll_step * 20) elapsed = scroll_step * 20;
			last_input_poll = now;
			int dx = 0, dy = 0;
			if (!is_drag_selecting) {
				if (wnd.get_key_state(81)) ++dy;
				else if (wnd.get_key_state(82)) --dy;
				// With the full graph pane open, left/right switch its tabs instead.
				if (!graph_arrows_switch_tabs()) {
					if (wnd.get_key_state(79)) ++dx;
					else if (wnd.get_key_state(80)) --dx;
				}
				// Edge pan: cursor within edge_pan_px of the window edge (only while the
				// cursor is over this window, so leaving it doesn't keep scrolling).
				if (edge_pan && !is_moving_minimap && !is_moving_replay_slider && wnd.has_mouse_focus()) {
					int cx = -1, cy = -1;
					wnd.get_cursor_pos(&cx, &cy);
					if (cx >= 0 && cy >= 0) {
						// Edges of the game view, not the window (the bars are outside it).
						if (cx < viewport_x + edge_pan_px) --dx;
						else if (cx >= viewport_x + viewport_w - edge_pan_px) ++dx;
						if (cy < edge_pan_px) --dy;
						else if (cy >= window_height - edge_pan_px) ++dy;
					}
				}
			}
			if (dx || dy) {
				scroll_speed_n = std::min(scroll_speeds.size() - 1, (size_t)(scroll_held / scroll_step));
				double steps = std::chrono::duration<double>(elapsed) / scroll_step;
				scroll_held += elapsed;
				double speed = scroll_speeds[scroll_speed_n] * steps * scroll_speed_factor();
				scroll_rem_x += dx * speed;
				scroll_rem_y += dy * speed;
				int mx = (int)scroll_rem_x, my = (int)scroll_rem_y;
				screen_pos.x += mx;
				screen_pos.y += my;
				scroll_rem_x -= mx;
				scroll_rem_y -= my;
			} else {
				scroll_speed_n = 0;
				scroll_held = {};
				scroll_rem_x = scroll_rem_y = 0;
			}

			if (is_moving_minimap) {
				int x = -1;
				int y = -1;
				wnd.get_cursor_pos(&x, &y);
				if (x != -1) move_minimap((int)((x - viewport_x) / zoom), (int)(y / zoom));
			}
			if (is_moving_replay_slider) {
				int x = -1;
				int y = -1;
				wnd.get_cursor_pos(&x, &y);
				if (x != -1) move_replay_slider((int)((x - viewport_x) / zoom), (int)(y / zoom));
			}
		}

		observer_step(now, screen_pos != camera_before_input);
		if (screen_pos.y + view_height > game_st.map_height) screen_pos.y = game_st.map_height - view_height;
		if (screen_pos.y < 0) screen_pos.y = 0;
		if (screen_pos.x + view_width > game_st.map_width) screen_pos.x = game_st.map_width - view_width;
		if (screen_pos.x < 0) screen_pos.x = 0;
		observer_last_screen_pos = screen_pos;

		const bool hd_frame = hd_ready();
		minimap_hd = hd_frame;
		auto drag_rect = [&] {
			auto r = rect{{drag_select_from_x, drag_select_from_y}, {drag_select_to_x, drag_select_to_y}};
			if (r.from.x > r.to.x) std::swap(r.from.x, r.to.x);
			if (r.from.y > r.to.y) std::swap(r.from.y, r.to.y);
			return r;
		};

		uint8_t* data = (uint8_t*)indexed_surface->lock();
#ifdef OPENBW_REMASTERED
		// Remastered: map and sprites go to the canvas; the indexed buffer only collects
		// the overlay (health bars, minimap, callback drawing) over a transparent sentinel.
		uint8_t overlay_sentinel = 0;
		if (hd_frame) {
			hd_begin_frame();
			hd_draw_tiles();
			overlay_sentinel = hd_overlay_sentinel();
			std::memset(data, overlay_sentinel, (size_t)indexed_surface->pitch * indexed_surface->h);
		}
#endif
		if (!hd_frame) draw_tiles(data, indexed_surface->pitch);
		hd_drawing = hd_frame;
		draw_sprites(data, indexed_surface->pitch);
		hd_drawing = false;
#ifdef OPENBW_REMASTERED
		if (hd_frame) {
			hd_flush();
			if (show_fog) {
				// Health bars drawn with the sprites sit under the fog, as in the classic view.
				hd_composite_overlay(data, indexed_surface->pitch, overlay_sentinel, rect{});
				std::memset(data, overlay_sentinel, (size_t)indexed_surface->pitch * indexed_surface->h);
				hd_draw_fog();
			}
		}
#endif
		if (!hd_frame) draw_fog(data, indexed_surface->pitch);

		draw_callback(data, indexed_surface->pitch);

		if (draw_ui_elements) draw_ui(data, indexed_surface->pitch);
#ifdef OPENBW_REMASTERED
		if (hd_frame) {
			hd_composite_overlay(data, indexed_surface->pitch, overlay_sentinel, rect{});
			if (is_drag_selecting) hd_draw_rect_outline(drag_rect(), 0xff18fc10);
			hd_end_frame();
		}
#endif
		indexed_surface->unlock();

		if (!hd_frame) {
			rgba_surface->fill(0, 0, 0, 255);
			indexed_surface->blit(&*rgba_surface, 0, 0);

			draw_image_queue();

			if (is_drag_selecting) {
				uint32_t* data = (uint32_t*)rgba_surface->lock();
				auto r = drag_rect();
				outline_rect<uint32_t>(data, rgba_surface->pitch / 4, r.from, r.to, 0xff18fc10);
				rgba_surface->unlock();
			}
		}

		if (wnd) {
			if (viewport_x > 0) window_surface->fill(0, 0, 0, 255);
#ifdef OPENBW_REMASTERED
			if (hd_frame) {
				hd_surface->blit(&*window_surface, viewport_x, 0);
			} else
#endif
			if (rgba_surface->w == viewport_w && rgba_surface->h == window_surface->h) {
				rgba_surface->blit(&*window_surface, viewport_x, 0);
			} else {
				rgba_surface->blit_scaled(&*window_surface, viewport_x, 0, viewport_w, window_surface->h);
			}
			if (show_hud) {
				draw_hud();
				if (hud_surface) hud_surface->blit(&*window_surface, viewport_x + 8, 8);
			}
			if (draw_ui_elements) {
				draw_minimap_window();
				draw_replay_bar();
			}
			if (graphs_visible()) draw_graph_panel();
			panel_area_window = rect{};
			if (show_selection_panel && draw_selection_panel()) {
				// Centred, but kept between the minimap and the replay panel where they allow.
				int px = viewport_x + (viewport_w - panel_surface->w) / 2;
				int right = draw_ui_elements && replay_st.end_frame ? replay_panel().panel.from.x - 8 : viewport_x + viewport_w;
				int left = draw_ui_elements ? minimap_window_rect().to.x + 8 : viewport_x;
				px = std::max(left, std::min(px, right - panel_surface->w));
				px = std::max(viewport_x, px);
				int py = std::max(0, window_surface->h - panel_surface->h - 8);
				panel_area_window = rect{{px, py}, {px + panel_surface->w, py + panel_surface->h}};
				panel_surface->blit(&*window_surface, px, py);
				draw_panel_tip();
			}
			if (show_settings_menu) draw_settings_menu();
			save_settings_if_changed();
			wnd.update_surface();
		}
	}

	std::tuple<int, int, uint32_t*> get_rgba_buffer() {
		void* r = rgba_surface->lock();
		rgba_surface->unlock();
		return std::make_tuple(rgba_surface->pitch / 4, rgba_surface->h, (uint32_t*)r);
	}

	template<typename cb_F>
	void async_read_file(a_string filename, cb_F cb) {
#ifdef EMSCRIPTEN
		auto uptr = std::make_unique<cb_F>(std::move(cb));
		auto f = [](void* ptr, uint8_t* data, size_t size) {
			cb_F* cb_p = (cb_F*)ptr;
			std::unique_ptr<cb_F> uptr(cb_p);
			(*cb_p)(data, size);
		};
		auto* a = filename.c_str();
		auto* b = (void(*)(void*, uint8_t*, size_t))f;
		auto* c = uptr.release();
		EM_ASM_({js_download_file($0, $1, $2);}, a, b, c);
#else
		filename = "data/" + filename;
		FILE* f = fopen(filename.c_str(), "rb");
		if (!f) {
			cb(nullptr, 0);
		} else {
			a_vector<uint8_t> data;
			fseek(f, 0, SEEK_END);
			data.resize(ftell(f));
			fseek(f, 0, SEEK_SET);
			data.resize(fread(data.data(), 1, data.size(), f));
			fclose(f);
			cb(data.data(), data.size());
		}
#endif
	}

	std::array<tileset_image_data, 8> all_tileset_img;

	template<typename load_data_file_F>
	void load_all_image_data(load_data_file_F&& load_data_file) {
		load_image_data(img, load_data_file);
		for (size_t i = 0; i != 8; ++i) {
			load_tileset_image_data(all_tileset_img[i], i, load_data_file);
		}
	}

	void set_image_data() {
		tileset_img = all_tileset_img.at(game_st.tileset_index);

		if (!palette) palette = native_window_drawing::new_palette();

		native_window_drawing::color palette_colors[256];
		if (tileset_img.wpe.size() != 256 * 4) error("wpe size invalid (%d)", tileset_img.wpe.size());
		for (size_t i = 0; i != 256; ++i) {
			palette_colors[i].r = tileset_img.wpe[4 * i + 0];
			palette_colors[i].g = tileset_img.wpe[4 * i + 1];
			palette_colors[i].b = tileset_img.wpe[4 * i + 2];
			palette_colors[i].a = tileset_img.wpe[4 * i + 3];
		}
		palette->set_colors(palette_colors);
	}

	void reset() {
		apm = {};
		graph_samples.clear();
		unit_deaths.clear();
		observer_damage.clear();
		observer_last_frame = -1;
		observer_has_target = false;
		observer_visits.clear();
		replay_frame = 0;
		auto& game = *st.game;
		st = state();
		game = game_state();
		replay_st = replay_state();
		action_st = action_state();

		st.global = &global_st;
		st.game = &game;
	}
};

}

