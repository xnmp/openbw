#include "common.h"
#include "../bwgame.h"
#include "../replay.h"

#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"
#include "hud_font.h"
#include "order_names.h"

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

		size_t color_index = st.players[sprite->owner].color;
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
		const unit_t* draw_health_bars_u = draw_selection_u;
		for (auto* image : ptr(reverse(sprite->images))) {
			if (i_flag(image, image_t::flag_hidden)) continue;
			if (draw_selection_u && image->modifier != 10) {
				draw_selection_circle(sprite, draw_selection_u, data, data_pitch);
				draw_selection_u = nullptr;
			}
			draw_image(image, data, data_pitch, st.players[sprite->owner].color);
		}
		if (draw_health_bars_u && !u_invincible(draw_health_bars_u)) {
			draw_health_bars(sprite, draw_health_bars_u, data, data_pitch);
		}
	}

	a_vector<std::pair<uint32_t, const sprite_t*>> sorted_sprites;

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

		std::sort(sorted_sprites.begin(), sorted_sprites.end());

		for (auto uid : current_selection) {
			auto* u = get_unit(uid);
			if (!u) continue;
			current_selection_sprites_set.at(u->sprite->index) = u;
			current_selection_sprites.push_back(u->sprite);
		}

		for (auto& v : sorted_sprites) {
			draw_sprite(v.second, data, data_pitch);
		}

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

	void line_rectangle(uint8_t* data, size_t data_pitch, rect area, uint8_t index) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t height = area.to.y - area.from.y;
		uint8_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;
		memset(p, index, width);
		memset(p + data_pitch * height, index, width);
		for (size_t y = 0; y != height; ++y) {
			p[data_pitch * y] = index;
			p[data_pitch * y + width - 1] = index;
		}
	}

	void line_rectangle_rgba(uint32_t* data, size_t data_pitch, rect area, uint32_t color) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t height = area.to.y - area.from.y;
		uint32_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;
		for (size_t x = 0; x != width; ++x) {
			p[x] = color;
			p[height * data_pitch + x] = color;
		}
		for (size_t y = 0; y != height; ++y) {
			p[data_pitch * y] = color;
			p[data_pitch * y + width - 1] = color;
		}
	}

	bool unit_visble_on_minimap(unit_t* u) {
		if (u->owner < 8 && u->sprite->visibility_flags == 0) return false;
		if (ut_turret(u)) return false;
		if (unit_is_trap(u)) return false;
		if (unit_is(u, UnitTypes::Spell_Dark_Swarm)) return false;
		if (unit_is(u, UnitTypes::Spell_Disruption_Web)) return false;
		return true;
	}

	rect get_minimap_area() {
		size_t minimap_width = std::max(game_st.map_tile_width, game_st.map_tile_height);
		size_t minimap_height = std::max(game_st.map_tile_width, game_st.map_tile_height);
		if (game_st.map_width < game_st.map_height) {
			minimap_width = minimap_width * minimap_width * game_st.map_tile_width / (minimap_height * game_st.map_tile_height);
		} else if (game_st.map_height < game_st.map_width) {
			minimap_height = minimap_height * minimap_height * game_st.map_tile_height / (minimap_width* game_st.map_tile_width);
		}
		if (screen_width < minimap_width || screen_height < minimap_height) return {};
		int map_screen_x = 4;
		int map_screen_y = screen_height - 4 - minimap_height;
		rect area;
		area.from = {map_screen_x, map_screen_y};
		area.to = area.from + xy{(int)minimap_width, (int)minimap_height};
		return area;
	}

	void draw_minimap(uint8_t* data, size_t data_pitch) {
		auto area = get_minimap_area();
		size_t minimap_width = area.to.x - area.from.x;
		size_t minimap_height = area.to.y - area.from.y;
		if (minimap_width != game_st.map_tile_width) return;
		if (minimap_height != game_st.map_tile_height) return;
		fill_rectangle(data, data_pitch, area, 0);
		line_rectangle(data, data_pitch, {area.from - xy(1, 1), area.to + xy(1, 1)}, 0);

		uint8_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;

		size_t pitch = data_pitch - game_st.map_tile_width;
		for (size_t y = 0; y != game_st.map_tile_height; ++y) {
			for (size_t x = 0; x != game_st.map_tile_width; ++x) {
				size_t index;
				if (~st.tiles[y * game_st.map_tile_width + x].flags & tile_t::flag_has_creep) index = st.tiles_mega_tile_index[y * game_st.map_tile_width + x];
				else index = game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[y * game_st.map_tile_width + x]];
				auto* images = &tileset_img.vx4.at(index).images[0];
				auto* bitmap = &tileset_img.vr4.at(*images / 2).bitmap[0];
				auto val = bitmap[55 / sizeof(vr4_entry::bitmap_t)];
				size_t shift = 8 * (55 % sizeof(vr4_entry::bitmap_t));
				val >>= shift;
				*p++ = (uint8_t)val;
			}
			p += pitch;
		}

		for (size_t i = 12; i != 0;) {
			--i;
			for (unit_t* u : ptr(st.player_units[i])) {
				if (!unit_visble_on_minimap(u)) continue;
				int color = img.player_minimap_colors.at(st.players[u->owner].color);
				size_t w = u->unit_type->placement_size.x / 32u;
				size_t h = u->unit_type->placement_size.y / 32u;
				if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
					color = tileset_img.resource_minimap_color;
				}
				if (ut_building(u)) {
					if (w > 4) w = 4;
					if (h > 4) h = 4;
				}
				if (w < 2) w = 2;
				if (h < 2) h = 2;
				rect unit_area;
				unit_area.from = area.from + (u->sprite->position - u->unit_type->placement_size / 2) / 32u;
				unit_area.to = unit_area.from + xy(w, h);
				fill_rectangle(data, data_pitch, unit_area, color);
			}
		}

		rect view_rect;
		view_rect.from = area.from + xy(screen_pos.x / 32u, screen_pos.y / 32u);
		view_rect.to = view_rect.from + xy((view_width + 31) / 32u, (view_height + 31) / 32u);
		line_rectangle(data, data_pitch, view_rect, 255);

	}

	int replay_frame = 0;

	rect get_replay_slider_area() {
#ifdef EMSCRIPTEN
		return {};
#endif
		rect r;
		int width = 192;
		int height = 32;
		r.from.x = (int)screen_width - 8 - width;
		r.from.y = (int)screen_height - 8 - 128 - height;
		r.to.x = r.from.x + width;
		r.to.y = r.from.y + height;
		if (r.from.x < 0 || r.from.y < 0) return {};
		return r;
	}

	void draw_ui(uint8_t* data, size_t data_pitch) {
		auto area = get_replay_slider_area();
		if (area == rect{}) return;
		if (replay_st.end_frame == 0) return;
		fill_rectangle(data, data_pitch, area, 1);
		line_rectangle(data, data_pitch, area, 12);

		int button_w = 16;
		int button_h = 32;
		int ow = (area.to.x - area.from.x) - button_w;
		int ox = replay_frame * ow / replay_st.end_frame;

		if (st.current_frame != replay_frame) {
			int cox = st.current_frame * ow / replay_st.end_frame;
			line_rectangle(data, data_pitch, rect{area.from + xy(cox + button_w / 2, 0), area.from + xy(cox + button_w / 2 + 1, button_h)}, 50);
		}

		fill_rectangle(data, data_pitch, rect{area.from + xy(ox, 0), area.from + xy(ox, 0) + xy(button_w, button_h)}, 10);
		line_rectangle(data, data_pitch, rect{area.from + xy(ox, 0), area.from + xy(ox, 0) + xy(button_w, button_h)}, 51);

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
	size_t scroll_speed_n = 0;

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

	void step_zoom(int direction, int anchor_x = -1, int anchor_y = -1) {
		static const double steps[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0};
		const int n = sizeof(steps) / sizeof(steps[0]);
		int i = 0;
		while (i + 1 < n && steps[i] < zoom - 1e-9) ++i;
		i += direction;
		if (i < 0) i = 0;
		if (i >= n) i = n - 1;
		set_zoom(steps[i], anchor_x, anchor_y);
	}

	void step_speed(bool faster) {
		if (faster) {
			if (game_speed < fp8::integer(128)) game_speed *= 2;
		} else {
			if (game_speed > 2_fp8) game_speed /= 2;
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
				size_t ci = img.player_minimap_colors.at(pl.color);
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
	// bottom-centre of the game view. Wireframe pixels are palette *roles*, not colours:
	// 208-211 (Zerg: 216-219) are four hull sections and 192/193 the outline, which carries
	// the shield state on shielded units. The section damage colouring approximates Brood
	// War's: each section steps green -> yellow -> red as hp falls, in an order fixed per
	// unit by its wireframe_randomizer.
	bool show_selection_panel = true;
	bool selection_panel_data_loaded = false;
	grp_t wireframe_grp, group_wireframe_grp;
	a_vector<bool> group_wireframe_valid;  // grpwire.grp frames for buildings etc. are "BLANK" art
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

	wire_rgb wire_pixel_rgb(int index, const unit_vitals& v) const {
		if (index >= 208 && index <= 211) return wire_section_rgb(wire_section_state(index - 208, v.hp, v.randomizer));
		if (index >= 216 && index <= 219) return wire_section_rgb(wire_section_state(index - 216, v.hp, v.randomizer));
		if (index == 192 || index == 193) {
			if (v.has_shields) {
				// Outline as the shield bar: bright blue full, dim blue partial, grey when down.
				if (v.shields <= 0) return {72, 72, 84};
				bool full_layer = index == 192 ? true : v.shields > 0.5;
				return full_layer ? wire_rgb{56, 120, 252} : wire_rgb{36, 64, 150};
			}
			auto c = wire_section_rgb(overall_state(v.hp));
			if (index == 193) return {c.r * 3 / 4, c.g * 3 / 4, c.b * 3 / 4};
			return c;
		}
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
		size_t ci = img.player_minimap_colors.at(st.players[owner].color);
		return {tileset_img.wpe[4 * ci + 0], tileset_img.wpe[4 * ci + 1], tileset_img.wpe[4 * ci + 2]};
	}

	struct panel_line {
		a_string text;
		wire_rgb rgb{230, 230, 230};
	};

	static int pct(int done, int total) {
		if (total <= 0) return 0;
		return std::max(0, std::min(100, 100 * done / total));
	}

	a_string weapon_line(const char* slot, const unit_t* u, const weapon_type_t* w, int max_hits) const {
		int base = w->damage_amount, bonus = (int)weapon_damage_amount(w, u->owner).integer_part() - base;
		char buf[160];
		int hits = std::max(1, max_hits) * std::max(1, w->bullet_count);
		std::snprintf(buf, sizeof(buf), "%s: %s  %d%s%s  rng %d  cd %d", slot, stat_text(w->label).c_str(), base,
			bonus ? (a_string(" (+") + std::to_string(bonus).c_str() + ")").c_str() : "",
			hits > 1 ? (a_string(" x") + std::to_string(hits).c_str()).c_str() : "",
			weapon_max_range(u, w) / 32, w->cooldown);
		return buf;
	}

	a_vector<panel_line> unit_panel_lines(const unit_t* u) const {
		a_vector<panel_line> lines;
		char buf[160];
		auto add = [&](a_string s, wire_rgb c = {230, 230, 230}) { lines.push_back({std::move(s), c}); };
		const unit_type_t* ut = u->unit_type;
		add(stat_text((int)ut->id + 1), {255, 255, 255});
		add(player_label(u->owner), player_rgb(u->owner));
		auto v = vitals_of(u);
		std::snprintf(buf, sizeof(buf), "HP %d/%d", (int)u->hp.ceil().integer_part(), (int)ut->hitpoints.integer_part());
		add(buf, wire_section_rgb(overall_state(v.hp)));
		if (v.has_shields) {
			std::snprintf(buf, sizeof(buf), "Shields %d/%d  (armor %d)", (int)u->shield_points.integer_part(), ut->shield_points,
				player_upgrade_level(u->owner, UpgradeTypes::Protoss_Plasma_Shields));
			add(buf, {110, 160, 255});
		}
		if (ut_has_energy(u)) {
			std::snprintf(buf, sizeof(buf), "Energy %d/%d", (int)u->energy.integer_part(), (int)unit_max_energy(u).integer_part());
			add(buf, {200, 120, 255});
		}
		if (ut_resource(u)) {
			std::snprintf(buf, sizeof(buf), "%s %d", unit_is_mineral_field(u) ? "Minerals" : "Gas", u->building.resource.resource_count);
			add(buf, unit_is_mineral_field(u) ? wire_rgb{120, 200, 255} : wire_rgb{90, 230, 120});
		}
		if (u->owner != 11 && !ut_resource(u)) {
			int base = ut->armor, total = (int)unit_armor(u).integer_part();
			std::snprintf(buf, sizeof(buf), total != base ? "Armor %d (+%d)" : "Armor %d", base, total - base);
			add(buf);
		}
		const unit_t* armed = u->subunit ? u->subunit : u;
		if (auto* w = unit_ground_weapon(armed)) add(weapon_line("Ground", u, w, armed->unit_type->max_ground_hits));
		if (auto* w = unit_air_weapon(armed)) {
			if (w != unit_ground_weapon(armed)) add(weapon_line("Air", u, w, armed->unit_type->max_air_hits));
		}
		if (unit_ground_weapon(armed) || unit_air_weapon(armed) || u->kill_count) {
			std::snprintf(buf, sizeof(buf), "Kills %d", u->kill_count);
			add(buf);
		}
		if (ut_worker(u) && u->worker.resources_carried && u->carrying_flags) {
			std::snprintf(buf, sizeof(buf), "Carrying %d %s", u->worker.resources_carried, (u->carrying_flags & 1) ? "gas" : "minerals");
			add(buf);
		}
		// Progress: construction/morph, training queue, research, upgrade.
		if (!u_completed(u) && ut->build_time > 0) {
			std::snprintf(buf, sizeof(buf), "Building %d%%", pct(ut->build_time - u->remaining_build_time, ut->build_time));
			add(buf, {255, 210, 90});
		} else if (!u->build_queue.empty() && u->build_queue.front()) {
			const unit_type_t* q = u->build_queue.front();
			const unit_t* b = u->current_build_unit;
			int done = b ? q->build_time - b->remaining_build_time : 0;
			std::snprintf(buf, sizeof(buf), "Training %s %d%%", stat_text((int)q->id + 1).c_str(), pct(done, q->build_time));
			add(buf, {255, 210, 90});
			if (u->build_queue.size() > 1) {
				a_string rest = "Queue:";
				for (size_t i = 1; i != u->build_queue.size(); ++i) rest += " " + stat_text((int)u->build_queue[i]->id + 1);
				add(rest, {200, 180, 120});
			}
		}
		if (ut_building(u) && u->building.researching_type) {
			auto* t = u->building.researching_type;
			std::snprintf(buf, sizeof(buf), "Researching %s %d%%", stat_text(t->label).c_str(), pct(t->research_time - u->building.upgrade_research_time, t->research_time));
			add(buf, {255, 210, 90});
		}
		if (ut_building(u) && u->building.upgrading_type) {
			auto* t = u->building.upgrading_type;
			int total = upgrade_time_cost(u->owner, t);
			std::snprintf(buf, sizeof(buf), "Upgrading %s %d %d%%", stat_text(t->label).c_str(), u->building.upgrading_level, pct(total - u->building.upgrade_research_time, total));
			add(buf, {255, 210, 90});
		}
		if (u->order_type && (int)u->order_type->id >= 0 && (int)u->order_type->id < order_names::count) {
			add(a_string("Order: ") + order_names::names[(int)u->order_type->id], {170, 180, 200});
		}
		a_string status;
		auto flag = [&](bool on, const char* name) { if (on) status += status.empty() ? name : (a_string(", ") + name).c_str(); };
		flag(u_hallucination(u), "Hallucination");
		flag(u_cloaked(u) || u_requires_detector(u), "Cloaked");
		flag(u_burrowed(u), "Burrowed");
		flag(u->stim_timer != 0, "Stimmed");
		flag(u->defensive_matrix_timer != 0, "Matrix");
		flag(u->ensnare_timer != 0, "Ensnared");
		flag(u->lockdown_timer != 0, "Lockdown");
		flag(u->irradiate_timer != 0, "Irradiated");
		flag(u->plague_timer != 0, "Plagued");
		flag(u->stasis_timer != 0, "Stasis");
		flag(u->maelstrom_timer != 0, "Maelstrom");
		flag(u->acid_spore_count != 0, "Acid spores");
		flag(u->parasite_flags != 0, "Parasite");
		if (!status.empty()) add(status, {255, 140, 120});
		int cargo = 0;
		for (auto id : u->loaded_units) if (id.raw_value && get_unit(id)) ++cargo;
		if (cargo) {
			std::snprintf(buf, sizeof(buf), "Cargo %d", cargo);
			add(buf);
		}
		return lines;
	}

	void prepare_panel_surface(int w, int h) {
		if (!panel_surface || panel_surface->w != w || panel_surface->h != h) {
			panel_surface = native_window_drawing::create_rgba_surface(w, h);
			panel_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
	}

	// Returns false when there is nothing to show. Panel scale follows window height so
	// wireframes stay legible on large screens.
	bool draw_selection_panel() {
		if (!selection_panel_data_loaded) return false;
		auto units = selected_units();
		if (units.empty()) return false;
		const int cw = hud_font::cell_w, ch = hud_font::cell_h, pad = 8;
		const int scale = window_height >= 1000 ? 2 : 1;
		if (units.size() == 1) {
			unit_t* u = units[0];
			auto lines = unit_panel_lines(u);
			int wire = 64 * scale;
			size_t text_cols = 0;
			for (auto& l : lines) text_cols = std::max(text_cols, l.text.size());
			int w = pad * 3 + wire + (int)text_cols * cw;
			int h = pad * 2 + std::max(wire, (int)lines.size() * ch);
			prepare_panel_surface(w, h);
			uint32_t* buf = (uint32_t*)panel_surface->lock();
			size_t pitch = panel_surface->pitch / 4;
			for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) buf[(size_t)y * pitch + x] = hud_rgba(8, 10, 16, 190);
			draw_wireframe(buf, pitch, w, h, pad, pad, wireframe_grp, (size_t)u->unit_type->id, scale, 1, vitals_of(u));
			for (size_t i = 0; i != lines.size(); ++i) {
				auto& l = lines[i];
				hud_text(buf, pitch, w, h, pad * 2 + wire, pad + (int)i * ch, l.text.c_str(), l.rgb.r, l.rgb.g, l.rgb.b);
			}
			panel_surface->unlock();
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
			auto pc = player_rgb(u->owner);
			for (int k = 0; k != cell - 2; ++k) {  // owner-coloured underline per cell
				int X = x + k, Y = y + cell - 3;
				if (X < w && Y < h) buf[(size_t)Y * pitch + X] = hud_rgba(pc.r, pc.g, pc.b, 255);
			}
			size_t id = (size_t)u->unit_type->id;
			if (id < group_wireframe_valid.size() && group_wireframe_valid[id]) {
				draw_wireframe(buf, pitch, w, h, x + 1, y, group_wireframe_grp, id, scale, 1, vitals_of(u));
			} else {
				draw_wireframe(buf, pitch, w, h, x + 1, y, wireframe_grp, id, scale, 2, vitals_of(u));
			}
		}
		panel_surface->unlock();
		return true;
	}

	// ---- Replay speed indicator, drawn crisp just above the progress bar ----
	std::unique_ptr<native_window_drawing::surface> speed_surface;

	// 1x is OpenBW's normal replay rate (42 ms per frame, Brood War's "Fastest").
	a_string speed_label() const {
		char buf[48];
		int raw = (int)game_speed.raw_value, one = (int)fp8::integer(1).raw_value;
		if (raw >= one) std::snprintf(buf, sizeof(buf), "%dx", raw / one);
		else std::snprintf(buf, sizeof(buf), "1/%dx", one / std::max(1, raw));
		return is_paused ? a_string("Paused  ") + buf : a_string("Speed ") + buf;
	}

	void draw_speed_indicator() {
		auto area = get_replay_slider_area();
		if (area == rect{} || replay_st.end_frame == 0) return;
		a_string text = speed_label();
		const int pad = 4;
		int w = (int)text.size() * hud_font::cell_w + pad * 2, h = hud_font::cell_h + pad;
		if (!speed_surface || speed_surface->w != w || speed_surface->h != h) {
			speed_surface = native_window_drawing::create_rgba_surface(w, h);
			speed_surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
		}
		uint32_t* buf = (uint32_t*)speed_surface->lock();
		size_t pitch = speed_surface->pitch / 4;
		for (int y = 0; y != h; ++y) for (int x = 0; x != w; ++x) buf[(size_t)y * pitch + x] = hud_rgba(8, 10, 16, 175);
		if (is_paused) hud_text(buf, pitch, w, h, pad, pad / 2, text.c_str(), 255, 200, 90);
		else hud_text(buf, pitch, w, h, pad, pad / 2, text.c_str(), 235, 235, 235);
		speed_surface->unlock();
		// Slider is in logical (pre-zoom) coordinates; right-align above it in window pixels.
		int right = viewport_x + (int)(area.to.x * zoom);
		int top = (int)(area.from.y * zoom);
		speed_surface->blit(&*window_surface, std::max(viewport_x, right - w), std::max(0, top - h - 4));
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

	void update() {
		auto now = clock.now();

		if (now - last_fps >= std::chrono::seconds(1)) {
			//ui::log("draw fps: %g\n", fps_counter / std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1, 1>>>(now - last_fps).count());
			last_fps = now;
			fps_counter = 0;
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
			int x = mouse_x - replay_slider_area.from.x;
			int button_w = 16;
			x -= button_w / 2;
			int ow = (replay_slider_area.to.x - replay_slider_area.from.x) - button_w;
			if (x < 0) x = 0;
			if (x >= ow) x = ow - 1;
			replay_frame = x * replay_st.end_frame / ow;
		};

		auto check_move_replay_slider = [&](auto& e) {
			if (e.mouse_x >= replay_slider_area.from.x && e.mouse_x < replay_slider_area.to.x) {
				if (e.mouse_y >= replay_slider_area.from.y && e.mouse_y < replay_slider_area.to.y) {
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

		if (wnd) {
			native_window::event_t e;
			while (wnd.peek_message(e)) {
				if (e.type == native_window::event_t::type_mouse_wheel) {
					// mouse_x/y are window pixels here (the anchor), not logical ones.
					step_zoom(e.wheel_y > 0 ? +1 : -1, std::max(0, std::min(viewport_w - 1, e.mouse_x - viewport_x)), e.mouse_y);
					continue;
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
					if (e.sym == 'h') show_hud = !show_hud;
					if (e.sym == 'i') show_selection_panel = !show_selection_panel;
					if (e.sym == 'm') {
						if (global_volume) {
							unmuted_volume = global_volume;
							set_volume(0);
						} else set_volume(unmuted_volume);
					}
					if (e.sym == 'a' || e.sym == 'u') step_speed(true);
					if (e.sym == 'z' || e.sym == 'd') step_speed(false);
					if (e.sym == '\b') {
						int t = 5 * 42 / 1000;
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
			auto input_poll_speed = std::chrono::milliseconds(12);

			auto input_poll_t = now - last_input_poll;
			while (input_poll_t >= input_poll_speed) {
				if (input_poll_t >= input_poll_speed * 20) last_input_poll = now - input_poll_speed;
				else last_input_poll += input_poll_speed;
				std::array<int, 6> scroll_speeds = {2, 2, 4, 6, 6, 8};

				if (!is_drag_selecting) {
					int scroll_speed = scroll_speeds[scroll_speed_n];
					auto prev_screen_pos = screen_pos;
					if (wnd.get_key_state(81)) screen_pos.y += scroll_speed;
					else if (wnd.get_key_state(82)) screen_pos.y -= scroll_speed;
					if (wnd.get_key_state(79)) screen_pos.x += scroll_speed;
					else if (wnd.get_key_state(80)) screen_pos.x -= scroll_speed;
					// Edge pan: cursor within edge_pan_px of the window edge (only while the
					// cursor is over this window, so leaving it doesn't keep scrolling).
					if (edge_pan && !is_moving_minimap && !is_moving_replay_slider && wnd.has_mouse_focus()) {
						int cx = -1, cy = -1;
						wnd.get_cursor_pos(&cx, &cy);
						if (cx >= 0 && cy >= 0) {
							if (cx < edge_pan_px) screen_pos.x -= scroll_speed;
							else if (cx >= window_width - edge_pan_px) screen_pos.x += scroll_speed;
							if (cy < edge_pan_px) screen_pos.y -= scroll_speed;
							else if (cy >= window_height - edge_pan_px) screen_pos.y += scroll_speed;
						}
					}
					if (screen_pos != prev_screen_pos) {
						if (scroll_speed_n != scroll_speeds.size() - 1) ++scroll_speed_n;
					} else scroll_speed_n = 0;
				}

				input_poll_t = now - last_input_poll;
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

		if (screen_pos.y + view_height > game_st.map_height) screen_pos.y = game_st.map_height - view_height;
		if (screen_pos.y < 0) screen_pos.y = 0;
		if (screen_pos.x + view_width > game_st.map_width) screen_pos.x = game_st.map_width - view_width;
		if (screen_pos.x < 0) screen_pos.x = 0;

		uint8_t* data = (uint8_t*)indexed_surface->lock();
		draw_tiles(data, indexed_surface->pitch);
		draw_sprites(data, indexed_surface->pitch);

		draw_callback(data, indexed_surface->pitch);

		if (draw_ui_elements) {
			draw_minimap(data, indexed_surface->pitch);
			draw_ui(data, indexed_surface->pitch);
		}
		indexed_surface->unlock();

		rgba_surface->fill(0, 0, 0, 255);
		indexed_surface->blit(&*rgba_surface, 0, 0);

		draw_image_queue();

		if (is_drag_selecting) {
			uint32_t* data = (uint32_t*)rgba_surface->lock();
			if (is_drag_selecting) {
				auto r = rect{{drag_select_from_x, drag_select_from_y}, {drag_select_to_x, drag_select_to_y}};
				if (r.from.x > r.to.x) std::swap(r.from.x, r.to.x);
				if (r.from.y > r.to.y) std::swap(r.from.y, r.to.y);

				line_rectangle_rgba(data, rgba_surface->pitch / 4, r, 0xff18fc10);
			}
			rgba_surface->unlock();
		}

		if (wnd) {
			if (viewport_x > 0) window_surface->fill(0, 0, 0, 255);
			if (rgba_surface->w == viewport_w && rgba_surface->h == window_surface->h) {
				rgba_surface->blit(&*window_surface, viewport_x, 0);
			} else {
				rgba_surface->blit_scaled(&*window_surface, viewport_x, 0, viewport_w, window_surface->h);
			}
			if (show_hud) {
				draw_hud();
				if (hud_surface) hud_surface->blit(&*window_surface, viewport_x + 8, 8);
			}
			if (draw_ui_elements) draw_speed_indicator();
			if (show_selection_panel && draw_selection_panel()) {
				int px = viewport_x + (viewport_w - panel_surface->w) / 2;
				int py = window_surface->h - panel_surface->h - 8;
				panel_surface->blit(&*window_surface, std::max(viewport_x, px), std::max(0, py));
			}
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

