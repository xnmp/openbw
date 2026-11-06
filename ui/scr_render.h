#ifndef OPENBW_SCR_RENDER_H
#define OPENBW_SCR_RENDER_H

// Remastered graphics for the software viewer: a lazily filled cache of sprite frames
// and terrain megatiles pre-scaled to the current zoom, and the raster operations that
// composite them onto a window-resolution RGBA canvas.
//
// Compositing follows StarCraft: Remastered's own shaders (ShadersGLSL in its CASC store):
//  * team colour: mix(diffuse, diffuse * team, mask)               (util_team_color.glsl)
//  * shadows go to an effect buffer that later sprites overwrite by their alpha, and the
//    final colour is darkened by up to half: c * (1 - 0.5 * shadow) (util_effect.glsl)
//  * warp flash ("brighten"): white at the sprite's alpha        (sprite_brighten_frag.glsl)
// The deferred lighting pass (normal/specular maps, point lights) is not reproduced.

#include "scr_assets.h"
#include "scr_casc.h"
#include "scr_parallel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scr {

struct sprite_frame {
	image px;  // premultiplied RGBA at screen scale
	std::vector<uint8_t> mask;  // team colour mask, same size as px; empty if none
	// Top-left relative to the image's map position, in game pixels.
	double left = 0, left_flipped = 0, top = 0;
	uint64_t used = 0;  // the graphics frame number it was last asked for in
};

// A selection-panel wireframe (SC:R unit/wirefram/wirefram.dds.grp, one frame per unit type:
// eight square cells side by side). Matched against the classic wirefram.grp palette roles:
// cells 0-3 are the hull sections (roles 208-211; for Zerg the "heat" layers 216-219), 4 and
// 5 the outer and inner shield outline (roles 193, 192), 6 the line art; 7 is empty.
struct wireframe_layers {
	int size = 0;
	std::array<image, 7> cells;  // premultiplied, size x size
	uint32_t line_mean = 128;    // mean grey of the line art (cell 6) where it is solid
};

// Where an image's frames can reach, in game pixels relative to its map position, over every
// frame of its HD art (which often extends past the SD grp box).
struct sprite_bounds {
	double left = 0, right = 0, top = 0, bottom = 0;
};

struct graphics {
	// Pre-scaled sprite frames, megatiles and creep edges are kept up to this many bytes.
	size_t scaled_budget = (size_t)256 << 20;
	// Likewise for what is read from the store: block-compressed sprite atlases and tilesets
	// of both the HD2 and HD sets.
	size_t source_budget = (size_t)384 << 20;
	// Over a budget, end_frame() drops what was used least recently (for atlases, the set not
	// in use first) down to 3/4 of it, so dropping does not recur every frame. What the frame
	// just drawn used is kept even over budget: dropping it would only rebuild it next frame.
	size_t held_scaled_bytes() const { return scaled_bytes; }
	size_t held_source_bytes() const { return source_bytes(); }

	graphics() {
		tiers[0].prefix = "HD2/";
		tiers[1].prefix = "";
	}
	graphics(const graphics&) = delete;
	graphics& operator=(const graphics&) = delete;

	bool open(const std::string& install_dir, std::string& error) {
		if (!casc.open(install_dir, error)) return false;
		bytes sd;
		if (casc.read("SD/mainSD.anim", sd)) references = parse_sd_anim_references(sd);
		if (!casc.has(anim_path(0))) {
			error = "no HD2 graphics in this install (" + anim_path(0) + ")";
			return false;
		}
		return true;
	}

	// Tileset as named in the CASC store, e.g. "Jungle". Returns false if missing or
	// unreadable, in which case nothing of it is kept.
	bool set_tileset(const std::string& name) {
		tileset_name = name;
		return load_tileset(cur());
	}

	// Above 2x the full HD assets (4 texels per game pixel) are used, so that every zoom
	// level only ever shrinks the art: enlarging HD2 tiles one by one blurs them and shows
	// their seams. If the HD set is incomplete, HD2 stays in use. Both sets stay loaded (within
	// source_budget), so zooming back and forth across 2x does not read them again.
	void set_zoom(double z) {
		if (z == zoom) return;
		zoom = z;
		size_t want = z > 2.0 ? 1 : 0;
		if (want != active && casc.has(tiers[want].prefix + "anim/main_000.anim") &&
		    (tileset_name.empty() || load_tileset(tiers[want]))) {
			active = want;
		}
		clear_scaled();
	}
	double get_zoom() const { return zoom; }

	// Frees everything decoded (while the classic graphics are shown).
	void release() {
		for (auto& t : tiers) drop_tier(t);
		clear_scaled();
		wireframes = {};
		wireframes_read = false;
		scaled_wireframes.clear();
	}

	// Loads the atlases of these images.dat entries, reading them from the store in parallel.
	// Repeats and entries already loaded are skipped.
	void load(const std::vector<size_t>& image_ids) {
		std::vector<sprite_source*> todo;
		std::vector<std::string> paths;
		for (size_t id : image_ids) {
			size_t resolved;
			sprite_source* s = source_slot(id, resolved);
			if (!s || s->loaded) continue;
			s->loaded = true;
			todo.push_back(s);
			paths.push_back(anim_path(resolved));
		}
		parallel_for(todo.size(), 1, [&](size_t i) {
			try {
				read_source(paths[i], *todo[i]);
			} catch (...) {
				*todo[i] = sprite_source{};  // not loaded: retried when drawn
			}
		});
		for (auto* s : todo) account_source(*s);
	}

	// Scales these (images.dat entry, frame) pairs for the current zoom, in parallel, so that
	// sprite() finds them. Entries not yet load()ed are loaded one by one first.
	void prepare(const std::vector<std::pair<size_t, size_t>>& frames) {
		struct job {
			sprite_source* s;
			size_t frame;
			std::unique_ptr<sprite_frame> result;
		};
		std::vector<job> jobs;
		for (auto& p : frames) {
			sprite_source* s = load_source(p.first);
			if (!s || p.second >= s->anim.frames.size()) continue;
			if (s->scaled.size() != s->anim.frames.size()) s->scaled.resize(s->anim.frames.size());
			if (!s->scaled[p.second]) jobs.push_back({s, p.second, nullptr});
			else s->scaled[p.second]->used = frame_no;
		}
		auto key = [](const job& j) { return std::make_pair((uintptr_t)j.s, j.frame); };
		std::sort(jobs.begin(), jobs.end(), [&](const job& a, const job& b) { return key(a) < key(b); });
		jobs.erase(std::unique(jobs.begin(), jobs.end(), [&](const job& a, const job& b) { return key(a) == key(b); }), jobs.end());
		parallel_for(jobs.size(), 4, [&](size_t i) {
			try {
				jobs[i].result = scale_frame(*jobs[i].s, jobs[i].s->anim.frames[jobs[i].frame]);
			} catch (...) {
			}
		});
		for (auto& j : jobs) {
			if (!j.result) continue;
			j.result->used = frame_no;
			account(frame_bytes(*j.result));
			j.s->scaled[j.frame] = std::move(j.result);
		}
	}

	// Likewise for megatiles (cv5/vx4 indices).
	void prepare_megatiles(const std::vector<size_t>& indices) {
		size_t count = cur().megatiles.frames.size();
		size_megatile_cache(count);
		std::vector<size_t> todo;
		for (size_t i : indices) {
			if (i >= count) continue;
			megatile_used[i] = frame_no;
			if (!scaled_megatiles[i]) todo.push_back(i);
		}
		std::sort(todo.begin(), todo.end());
		todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
		std::vector<std::unique_ptr<image>> out(todo.size());
		parallel_for(todo.size(), 8, [&](size_t k) {
			try {
				out[k] = scale_megatile(todo[k]);
			} catch (...) {
			}
		});
		for (size_t k = 0; k != todo.size(); ++k) {
			if (!out[k]) continue;
			account(image_bytes(*out[k]));
			scaled_megatiles[todo[k]] = std::move(out[k]);
		}
	}

	// Bounds of every frame of images.dat entry `image_id`, or null if it has no HD graphics.
	// Loads the compressed atlas but decodes nothing, so callers can cull before sprite().
	const sprite_bounds* bounds(size_t image_id) {
		auto* s = load_source(image_id);
		return s ? &s->bounds : nullptr;
	}

	// The frame of images.dat entry `image_id`, or null if it has no HD graphics.
	const sprite_frame* sprite(size_t image_id, size_t frame) {
		auto* s = load_source(image_id);
		if (!s || frame >= s->anim.frames.size()) return nullptr;
		if (s->scaled.size() != s->anim.frames.size()) s->scaled.resize(s->anim.frames.size());
		auto& r = s->scaled[frame];
		if (!r) {
			r = scale_frame(*s, s->anim.frames[frame]);
			account(frame_bytes(*r));
		}
		r->used = frame_no;
		return r.get();
	}

	// Megatile `index` (cv5/vx4 index) as a tile_pixels()-wide square, or null.
	const image* megatile(size_t index) {
		size_t count = cur().megatiles.frames.size();
		if (index >= count) return nullptr;
		size_megatile_cache(count);
		auto& r = scaled_megatiles[index];
		if (!r) {
			r = scale_megatile(index);
			account(image_bytes(*r));
		}
		megatile_used[index] = frame_no;
		return r->empty() ? nullptr : r.get();
	}

	// Creep edge frame `index` scaled to `w` x `h` screen pixels (its SD rect at zoom). One
	// size is kept per frame: asking for another size in the same frame invalidates the
	// pointer returned earlier.
	const sprite_frame* creep_edge(size_t index, int w, int h) {
		auto& edges = cur().creep_edges;
		if (index >= edges.frames.size() || w <= 0 || h <= 0) return nullptr;
		if (scaled_creep.size() != edges.frames.size()) scaled_creep.resize(edges.frames.size());
		auto& r = scaled_creep[index];
		if (!r || r->w != w || r->h != h) {
			// A frame that fails to decode is cached as empty for this size, not retried.
			image src;
			if (r) scaled_bytes -= image_bytes(r->frame.px);
			r = std::make_unique<scaled_creep_edge>();
			r->w = w;
			r->h = h;
			if (edges.decode(index, src)) {
				premultiply(src);
				r->frame.px = resample(src, 0, 0, src.w, src.h, w, h, false);
			}
			account(image_bytes(r->frame.px));
		}
		r->frame.used = frame_no;
		return r->frame.px.empty() ? nullptr : &r->frame;
	}

	size_t creep_edge_count() const { return cur().creep_edges.frames.size(); }

	// Every frame of the .dds.grp at `path` (e.g. "HD2/cursor/arrow.dds.grp"), straight
	// alpha; empty if the install lacks it or a frame fails to decode.
	std::vector<image> read_frames(const std::string& path) {
		bytes b;
		dds_grp g;
		if (!casc.read(path, b) || !parse_dds_grp(std::move(b), g)) return {};
		std::vector<image> out(g.frames.size());
		for (size_t i = 0; i != out.size(); ++i) {
			if (!g.decode(i, out[i])) return {};
		}
		return out;
	}

	// The wireframe of unit type `unit_type` at `size` pixels square, or null if the install
	// lacks it. Valid until the next call (a few sizes and types are kept).
	const wireframe_layers* wireframe(size_t unit_type, int size) {
		if (!wireframes_read) {
			wireframes_read = true;
			bytes b;
			if (casc.read("HD2/unit/wirefram/wirefram.dds.grp", b)) parse_dds_grp(std::move(b), wireframes);
		}
		if (unit_type >= wireframes.frames.size() || size <= 0 || size > 1024) return nullptr;
		auto& r = scaled_wireframes[std::make_pair(unit_type, size)];
		if (!r) {
			if (scaled_wireframes.size() > 64) {
				scaled_wireframes.clear();
				return wireframe(unit_type, size);
			}
			r = std::make_unique<wireframe_layers>();
			image strip;
			int cell = wireframes.frames[unit_type].h;
			if (cell > 0 && wireframes.decode(unit_type, strip) && strip.w >= 7 * cell && strip.h >= cell) {
				premultiply(strip);
				r->size = size;
				for (int k = 0; k != 7; ++k) r->cells[k] = resample(strip, k * cell, 0, cell, cell, size, size, false);
				// A few wireframes (the Shuttle's) have no line art in the install, only their
				// hull sections, which are then filled shapes with their own shading: use
				// those sections as the line art, so the hull is drawn rather than just the
				// shield outline.
				// (Its line cell holds only a few faint stray pixels: "no line art" is less
				// than a twentieth of the sections' coverage.)
				auto& line = r->cells[6].px;
				uint64_t line_cover = 0, section_cover = 0;
				for (uint32_t p : line) line_cover += ch_a(p);
				for (int k = 0; k != 4; ++k) for (uint32_t p : r->cells[k].px) section_cover += ch_a(p);
				if (line_cover * 20 < section_cover) {
					for (size_t i = 0; i != line.size(); ++i) {
						for (int k = 0; k != 4; ++k) {
							uint32_t p = r->cells[k].px[i];
							if (ch_a(p) > ch_a(line[i])) line[i] = p;
						}
					}
				}
				uint64_t sum = 0, n = 0;
				for (uint32_t p : r->cells[6].px) {
					if (ch_a(p) <= 128) continue;
					sum += ch_r(p) * 255 / ch_a(p);
					++n;
				}
				if (n && sum) r->line_mean = (uint32_t)std::max<uint64_t>(1, sum / n);
			}
		}
		return r->size ? r.get() : nullptr;
	}

	// Call after each frame is drawn (and its draws run): applies the budgets.
	void end_frame() {
		if (source_bytes() > source_budget) evict_sources();
		if (scaled_bytes > scaled_budget) evict_scaled();
		++frame_no;
	}

	int tile_pixels() const { return (int)std::lround(32 * zoom); }

	// A .dds.grp from the store (e.g. "HD2/unit/cmdicons/cmdicons.dds.grp"), frames decoded on use.
	bool read_ui_grp(const std::string& path, dds_grp& out) const {
		bytes b;
		return casc.read(path, b) && parse_dds_grp(std::move(b), out);
	}

	// A standalone .dds from the store (UI art such as "HD2/game/observer/..."), premultiplied.
	bool read_ui_image(const std::string& path, image& out) const {
		bytes b;
		if (!casc.read(path, b) || !decode_dds(b.data(), b.size(), out)) return false;
		premultiply(out);
		return true;
	}

	// Megatile `index` of the current tileset averaged over a k x k grid (opaque, row-major),
	// for the minimap. False if the tileset lacks it or it does not decode.
	bool megatile_cells(size_t index, int k, std::vector<uint32_t>& out) const {
		image src;
		if (k <= 0 || !cur().megatiles.decode(index, src) || src.w < k || src.h < k) return false;
		out = resample(src, 0, 0, src.w, src.h, k, k, true).px;
		return true;
	}

private:
	struct sprite_source {
		bool loaded = false;
		uint64_t used = 0;  // frame number of the last use
		anim_entry anim;
		bytes diffuse;  // DDS files, still block-compressed
		bytes teamcolor;
		int atlas_w = 0, atlas_h = 0;
		sprite_bounds bounds;
		std::vector<std::unique_ptr<sprite_frame>> scaled;
	};
	struct scaled_creep_edge {
		int w = 0, h = 0;  // the requested size, kept even if decoding failed
		sprite_frame frame;
	};
	// One asset set: HD2 ("HD2/" prefix) or HD (no prefix).
	struct tier {
		std::string prefix;
		std::vector<std::unique_ptr<sprite_source>> sprites;
		std::string tileset_name;  // what megatiles/creep_edges hold (or failed to load)
		dds_grp megatiles, creep_edges;
		size_t bytes = 0;  // atlases and tileset data held
	};

	casc_storage casc;
	std::vector<int> references;
	tier tiers[2];
	size_t active = 0;  // index into tiers
	std::string tileset_name;
	std::vector<std::unique_ptr<image>> scaled_megatiles;
	std::vector<uint64_t> megatile_used;  // frame number of each one's last use
	std::vector<std::unique_ptr<scaled_creep_edge>> scaled_creep;
	dds_grp wireframes;  // HD2 set, which suits the panel's sizes
	bool wireframes_read = false;
	std::map<std::pair<size_t, int>, std::unique_ptr<wireframe_layers>> scaled_wireframes;
	double zoom = 1.0;
	size_t scaled_bytes = 0;
	uint64_t frame_no = 1;  // counts end_frame() calls; cache entries record their last use

	tier& cur() { return tiers[active]; }
	const tier& cur() const { return tiers[active]; }

	std::string anim_path(size_t image_id) const {
		char n[32];
		std::snprintf(n, sizeof(n), "anim/main_%03u.anim", (unsigned)image_id);
		return cur().prefix + n;
	}

	size_t source_bytes() const { return tiers[0].bytes + tiers[1].bytes; }

	bool load_tileset(tier& t) {
		if (t.tileset_name == tileset_name) return !t.megatiles.frames.empty();
		t.bytes -= t.megatiles.data.size() + t.creep_edges.data.size();
		t.megatiles = {};
		t.creep_edges = {};
		t.tileset_name = tileset_name;
		bytes b;
		if (casc.read(t.prefix + "TileSet/" + tileset_name + ".dds.vr4", b)) parse_dds_grp(std::move(b), t.megatiles);
		if (casc.read(t.prefix + "TileSet/" + tileset_name + ".dds.grp", b)) parse_dds_grp(std::move(b), t.creep_edges);
		t.bytes += t.megatiles.data.size() + t.creep_edges.data.size();
		if (&t == &cur()) clear_scaled();
		return !t.megatiles.frames.empty();
	}

	static size_t image_bytes(const image& i) { return i.px.size() * 4; }
	static size_t frame_bytes(const sprite_frame& f) { return image_bytes(f.px) + f.mask.size(); }

	// Frees an atlas and its scaled frames.
	void drop_source(tier& t, std::unique_ptr<sprite_source>& s) {
		if (!s) return;
		t.bytes -= s->diffuse.size() + s->teamcolor.size();
		for (auto& f : s->scaled) if (f) scaled_bytes -= frame_bytes(*f);
		s.reset();
	}

	void drop_tier(tier& t) {
		for (auto& s : t.sprites) drop_source(t, s);
		t.sprites.clear();
		t.tileset_name.clear();
		t.megatiles = {};
		t.creep_edges = {};
		t.bytes = 0;
	}

	void clear_scaled() {
		for (auto& t : tiers) for (auto& s : t.sprites) if (s) s->scaled.clear();
		scaled_megatiles.clear();
		megatile_used.clear();
		scaled_creep.clear();
		scaled_bytes = 0;
	}

	void size_megatile_cache(size_t count) {
		if (scaled_megatiles.size() != count) {
			for (auto& m : scaled_megatiles) if (m) scaled_bytes -= image_bytes(*m);
			scaled_megatiles.clear();
			scaled_megatiles.resize(count);
			megatile_used.assign(count, 0);
		}
	}

	// See source_budget. The set not in use goes first; then the current set's atlases, least
	// recently used first. Atlases that failed to load hold nothing and are kept, so they are
	// not read again.
	void evict_sources() {
		const size_t target = source_budget / 4 * 3;
		if (tiers[1 - active].bytes) drop_tier(tiers[1 - active]);
		if (source_bytes() <= target) return;
		auto& sprites = cur().sprites;
		std::vector<std::pair<uint64_t, size_t>> order;  // (last use, image id)
		for (size_t id = 0; id != sprites.size(); ++id) {
			auto& src = sprites[id];
			if (src && src->used != frame_no && !src->diffuse.empty()) order.emplace_back(src->used, id);
		}
		std::sort(order.begin(), order.end());
		for (auto& o : order) {
			if (source_bytes() <= target) break;
			drop_source(cur(), sprites[o.second]);
		}
	}

	// See scaled_budget: drops scaled frames, megatiles and creep edges, least recently used
	// first.
	void evict_scaled() {
		const size_t target = scaled_budget / 4 * 3;
		struct victim {
			uint64_t used;
			size_t bytes;
			std::unique_ptr<sprite_frame>* frame;
			std::unique_ptr<image>* megatile;
			std::unique_ptr<scaled_creep_edge>* creep;
		};
		std::vector<victim> v;
		for (auto& t : tiers) for (auto& src : t.sprites) if (src) for (auto& f : src->scaled) {
			if (f && f->used != frame_no) v.push_back({f->used, frame_bytes(*f), &f, nullptr, nullptr});
		}
		for (size_t i = 0; i != scaled_megatiles.size(); ++i) {
			auto& m = scaled_megatiles[i];
			if (m && megatile_used[i] != frame_no) v.push_back({megatile_used[i], image_bytes(*m), nullptr, &m, nullptr});
		}
		for (auto& c : scaled_creep) {
			if (c && c->frame.used != frame_no) v.push_back({c->frame.used, image_bytes(c->frame.px), nullptr, nullptr, &c});
		}
		std::sort(v.begin(), v.end(), [](const victim& a, const victim& b) { return a.used < b.used; });
		for (auto& x : v) {
			if (scaled_bytes <= target) break;
			if (!x.bytes) continue;  // an empty (failed) entry: dropping it frees nothing
			scaled_bytes -= x.bytes;
			if (x.frame) x.frame->reset();
			else if (x.megatile) x.megatile->reset();
			else x.creep->reset();
		}
	}

	void account(size_t n) { scaled_bytes += n; }

	void account_source(const sprite_source& s) {
		cur().bytes += s.diffuse.size() + s.teamcolor.size();
	}

	// The current set's slot for `image_id` after following SD references (whose id goes to
	// `resolved`), created empty if need be; null for ids past the table.
	sprite_source* source_slot(size_t image_id, size_t& resolved) {
		for (int hops = 0; hops != 4 && image_id < references.size() && references[image_id] >= 0; ++hops) {
			image_id = (size_t)references[image_id];
		}
		resolved = image_id;
		if (image_id >= 999) return nullptr;
		auto& sprites = cur().sprites;
		if (sprites.size() <= image_id) sprites.resize(image_id + 1);
		auto& s = sprites[image_id];
		if (!s) s = std::make_unique<sprite_source>();
		s->used = frame_no;
		return s.get();
	}

	sprite_source* load_source(size_t image_id) {
		size_t resolved;
		sprite_source* s = source_slot(image_id, resolved);
		if (!s) return nullptr;
		if (!s->loaded) {
			s->loaded = true;
			read_source(anim_path(resolved), *s);
			account_source(*s);
		}
		return s->diffuse.empty() ? nullptr : s;
	}

	// Reads an .anim file into `s`, keeping its diffuse and team colour atlases compressed.
	// Leaves s.diffuse empty if the file is missing or malformed. Safe to call on several
	// threads at once for different `s`.
	void read_source(const std::string& path, sprite_source& s) const {
		bytes f;
		dds_info info, team;
		if (!casc.read(path, f) || !parse_anim(f, s.anim) || !s.anim.diffuse.offset ||
		    !dds_header(f.data() + s.anim.diffuse.offset, s.anim.diffuse.size, info)) {
			return;
		}
		s.diffuse.assign(f.begin() + s.anim.diffuse.offset, f.begin() + s.anim.diffuse.offset + s.anim.diffuse.size);
		s.atlas_w = info.w;
		s.atlas_h = info.h;
		auto& t = s.anim.teamcolor;
		if (t.offset && dds_header(f.data() + t.offset, t.size, team) && team.w == info.w && team.h == info.h) {
			s.teamcolor.assign(f.begin() + t.offset, f.begin() + t.offset + t.size);
		}
		s.bounds = frame_bounds(s.anim);
	}

	// The frame table and entry box are in HD units (4 per game pixel) in every file; an
	// HD2 atlas has anim.scale = 2 texels per game pixel.
	static constexpr double units_per_pixel() { return 4.0; }

	static sprite_bounds frame_bounds(const anim_entry& a) {
		sprite_bounds b;
		bool first = true;
		for (auto& f : a.frames) {
			double l = (f.x - a.w / 2.0) / units_per_pixel(), t = (f.y - a.h / 2.0) / units_per_pixel();
			double r = l + f.w / units_per_pixel(), bt = t + f.h / units_per_pixel();
			b.left = first ? l : std::min(b.left, l);
			b.right = first ? r : std::max(b.right, r);
			b.top = first ? t : std::min(b.top, t);
			b.bottom = first ? bt : std::max(b.bottom, bt);
			first = false;
		}
		return b;
	}

	// A tile that fails to decode comes back as an empty image (cached, not retried).
	// Reads only immutable state, so may run on several threads at once.
	std::unique_ptr<image> scale_megatile(size_t index) const {
		auto r = std::make_unique<image>();
		image src;
		int n = tile_pixels();
		if (cur().megatiles.decode(index, src)) *r = resample(src, 0, 0, src.w, src.h, n, n, true);
		return r;
	}

	// Reads only immutable state, so may run on several threads at once.
	std::unique_ptr<sprite_frame> scale_frame(const sprite_source& s, const anim_frame& f) const {
		auto r = std::make_unique<sprite_frame>();
		const double units = units_per_pixel();
		double texels = s.anim.scale / units;
		int tx = (int)std::lround(f.tex_x * texels), ty = (int)std::lround(f.tex_y * texels);
		int fw = std::min((int)std::lround(f.w * texels), s.atlas_w - tx);
		int fh = std::min((int)std::lround(f.h * texels), s.atlas_h - ty);
		if (fw <= 0 || fh <= 0) return r;
		double k = zoom / s.anim.scale;  // screen pixels per texel
		int dw = std::max(1, (int)std::lround(fw * k)), dh = std::max(1, (int)std::lround(fh * k));
		// Real frames are at most a few hundred texels a side; refuse absurd ones.
		const double max_pixels = 4096.0 * 4096.0;
		if ((double)fw * fh > max_pixels || (double)dw * dh > max_pixels) return r;
		image src;
		if (!decode_dds_region(s.diffuse.data(), s.diffuse.size(), tx, ty, fw, fh, src)) return r;
		premultiply(src);
		r->px = resample(src, 0, 0, fw, fh, dw, dh, false);
		image m;
		if (!s.teamcolor.empty() && decode_dds_region(s.teamcolor.data(), s.teamcolor.size(), tx, ty, fw, fh, m)) {
			// Resample the mask (red channel) as an opaque grey image.
			for (auto& p : m.px) p = rgba(ch_r(p), ch_r(p), ch_r(p), 255);
			image ms = resample(m, 0, 0, fw, fh, dw, dh, true);
			r->mask.resize(ms.px.size());
			bool any = false;
			for (size_t i = 0; i != ms.px.size(); ++i) {
				r->mask[i] = (uint8_t)ch_r(ms.px[i]);
				any |= r->mask[i] != 0;
			}
			if (!any) r->mask.clear();
		}
		r->left = (f.x - s.anim.w / 2.0) / units;
		r->left_flipped = (s.anim.w / 2.0 - (f.x + f.w)) / units;
		r->top = (f.y - s.anim.h / 2.0) / units;
		return r;
	}
};

// ---- raster operations ----

// A rectangle [x0, x1) x [y0, y1); empty when x0 >= x1 or y0 >= y1.
struct box {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool empty() const { return x0 >= x1 || y0 >= y1; }
	void add(int ax0, int ay0, int ax1, int ay1) {
		if (empty()) *this = {ax0, ay0, ax1, ay1};
		else *this = {std::min(x0, ax0), std::min(y0, ay0), std::max(x1, ax1), std::max(y1, ay1)};
	}
};

struct canvas {
	uint32_t* px = nullptr;
	size_t pitch = 0;  // in pixels
	int w = 0, h = 0;
	uint8_t* shadow = nullptr;  // w * h effect buffer (shadow coverage), pitch w
	// If set, where shadow coverage was added since the last resolve_shadows(), which then
	// only visits that area; otherwise it visits the whole canvas.
	box* shadow_area = nullptr;
};

enum class blend {
	normal,  // alpha blend with team colour; clears shadow under it
	shadow,  // accumulate shadow coverage only
	brighten,  // white at the sprite's alpha (warp flash)
	tinted,  // alpha blend with the whole sprite multiplied by `team` (selection circles)
};

inline uint32_t mul255(uint32_t a, uint32_t b) {
	uint32_t t = a * b + 128;
	return (t + (t >> 8)) >> 8;
}

// Colours a wireframe: the line art takes the colour of the hull section it lies in (mixed
// where sections overlap), then the outer and inner shield outlines are laid over it in
// theirs. Colours are RGB; the result is premultiplied RGBA.
//
// Every race's line art is shaded relative to its own mean grey (see heat_shade).
// Zerg (`heat`): the four cells are nested heat masks, from the core (cell 0) out to the
// whole body (cell 3), so a pixel takes the innermost one covering it. Their line art is
// shaded grey rather than white: multiplying by it as is would leave the colours a dull
// brown, so its shade is taken relative to the wireframe's mean (up to 1.3x), which keeps
// the classic wireframe's flat, saturated reds, oranges and greens. (An overlay blend
// instead washes bright art, like the Spawning Pool's pool, out to white.)
inline uint32_t heat_shade(uint32_t base, uint32_t shade, uint32_t mean) {
	// Lift the darkest strokes and allow highlights up to 1.5x: the classic wireframes
	// are flat, bright colour, so shading here only adds texture.
	uint32_t k = std::max<uint32_t>(205, std::min<uint32_t>(384, shade * 256 / mean));  // x256
	return std::min<uint32_t>(255, base * k >> 8);
}

// `gain` (x256) scales the hull's brightness before the shield outlines go over it: the
// races' art differs in how much of it is lit (Zerg bodies are mostly dark red and orange,
// Protoss hulls nearly solid), so the caller can even them out.
inline image composite_wireframe(const wireframe_layers& w, const std::array<uint32_t, 4>& sections, uint32_t outer, uint32_t inner, bool heat = false, uint32_t gain = 256) {
	image out;
	out.w = out.h = w.size;
	out.px.assign((size_t)w.size * w.size, 0);
	for (size_t i = 0; i != out.px.size(); ++i) {
		uint32_t total = 0, r = 0, g = 0, b = 0;
		uint32_t inside = 0;  // heat: coverage of the cells inside this one
		for (int k = 0; k != 4; ++k) {
			uint32_t a = ch_a(w.cells[k].px[i]);
			uint32_t m = heat ? (a > inside ? a - inside : 0) : a;
			if (heat) inside = std::max(inside, a);
			total += m;
			r += m * ch_r(sections[k]);
			g += m * ch_g(sections[k]);
			b += m * ch_b(sections[k]);
		}
		if (heat && total) {
			// The masks are soft, so a weighted mix muddies the reds, oranges, yellows and
			// greens into brown; take the innermost mask that is solid here instead, giving
			// the classic wireframe's distinct bands.
			int pick = -1, best = -1;
			uint32_t best_a = 0;
			for (int k = 0; k != 4; ++k) {
				uint32_t a = ch_a(w.cells[k].px[i]);
				if (a >= 128 && pick < 0) pick = k;
				if (a > best_a) best_a = a, best = k;
			}
			if (pick < 0) pick = best;
			r = total * ch_r(sections[pick]);  // r / total is the colour; total keeps the coverage
			g = total * ch_g(sections[pick]);
			b = total * ch_b(sections[pick]);
		}
		uint32_t cr = 0, cg = 0, cb = 0, ca = 0;
		if (heat && total) {
			// Zerg heat masks cover the whole body: fill it, as the classic wireframes are
			// flat colour, and let the line art add texture over that rather than leave the
			// dark gaps between its strokes showing.
			uint32_t fa = std::min(255u, total);
			cr = mul255(r / total, fa);
			cg = mul255(g / total, fa);
			cb = mul255(b / total, fa);
			ca = fa;
		}
		uint32_t line = w.cells[6].px[i];
		if (total && ch_a(line)) {
			// How much of the line lies in some section. Terran and Protoss sections are
			// themselves thin, faint strokes tracing the line art (alpha under ~190, and
			// offset by antialiasing), so their coverage is lifted to make lines solid.
			uint32_t cover = std::min(255u, heat ? total : total * 4);
			uint32_t la = ch_a(line);
			uint32_t shade = std::min(255u, ch_r(line) * 255 / la);  // un-premultiplied grey
			// Thin lines resampled small keep little coverage; ease it up so small
			// wireframes (the selection grid) stay as bright as large ones.
			// Line art: thin strokes resampled small keep little alpha, so lift it steeply
			// (Terran and Protoss wireframes are all line, with no solid hull to carry them).
			uint32_t a = mul255(heat ? 255 - mul255(255 - la, 255 - la) : std::min(255u, la * 3), cover);
			if (!heat) shade = std::max(shade, w.line_mean * 4 / 5);  // floor the darkest strokes
			uint32_t keep = 255 - a;
			if (heat) shade = (w.line_mean * 3 + shade * 2) / 5;  // texture only: the fill carries the colour
			cr = mul255(heat_shade(r / total, shade, w.line_mean), a) + mul255(cr, keep);
			cg = mul255(heat_shade(g / total, shade, w.line_mean), a) + mul255(cg, keep);
			cb = mul255(heat_shade(b / total, shade, w.line_mean), a) + mul255(cb, keep);
			ca = a + mul255(ca, keep);
		}
		if (gain != 256) {  // premultiplied, so a colour channel may not exceed alpha
			cr = std::min(ca, cr * gain >> 8);
			cg = std::min(ca, cg * gain >> 8);
			cb = std::min(ca, cb * gain >> 8);
		}
		for (int k : {4, 5}) {
			uint32_t p = w.cells[k].px[i], tint = k == 4 ? outer : inner;
			uint32_t a = ch_a(p), ia = 255 - a;
			cr = mul255(ch_r(p), ch_r(tint)) + mul255(cr, ia);
			cg = mul255(ch_g(p), ch_g(tint)) + mul255(cg, ia);
			cb = mul255(ch_b(p), ch_b(tint)) + mul255(cb, ia);
			ca = a + mul255(ca, ia);
		}
		out.px[i] = rgba(cr, cg, cb, ca);
	}
	return out;
}

// Draws `f` with its top-left at canvas (x, y), mirrored if `flipped`. `alpha` (0..255)
// scales the sprite's coverage; `team` is the RGB team colour.
inline void draw_sprite(const canvas& c, const sprite_frame& f, int x, int y, bool flipped, blend mode, uint32_t team, uint32_t alpha) {
	int w = f.px.w, h = f.px.h;
	int x0 = std::max(0, -x), x1 = std::min(w, c.w - x);
	int y0 = std::max(0, -y), y1 = std::min(h, c.h - y);
	if (x0 >= x1 || y0 >= y1 || alpha == 0) return;
	if (mode == blend::shadow && c.shadow_area) c.shadow_area->add(x + x0, y + y0, x + x1, y + y1);
	uint32_t tr = ch_r(team), tg = ch_g(team), tb = ch_b(team);
	const bool has_mask = !f.mask.empty();
	for (int sy = y0; sy != y1; ++sy) {
		uint32_t* dst = c.px + (size_t)(y + sy) * c.pitch + x;
		uint8_t* sh = c.shadow + (size_t)(y + sy) * c.w + x;
		const uint32_t* src = &f.px.px[(size_t)sy * w];
		const uint8_t* msk = has_mask ? &f.mask[(size_t)sy * w] : nullptr;
		for (int sx = x0; sx != x1; ++sx) {
			int si = flipped ? w - 1 - sx : sx;
			uint32_t s = src[si];
			uint32_t a = ch_a(s);
			if (a == 0) continue;
			if (alpha != 255) {
				a = mul255(a, alpha);
				s = rgba(mul255(ch_r(s), alpha), mul255(ch_g(s), alpha), mul255(ch_b(s), alpha), a);
			}
			uint32_t& d = dst[sx];
			uint8_t& shd = sh[sx];
			switch (mode) {
			case blend::normal: {
				uint32_t r = ch_r(s), g = ch_g(s), b = ch_b(s);
				if (msk && msk[si]) {
					uint32_t m = msk[si], im = 255 - m;
					r = mul255(r, im + mul255(tr, m));
					g = mul255(g, im + mul255(tg, m));
					b = mul255(b, im + mul255(tb, m));
				}
				uint32_t ia = 255 - a;
				d = rgba(r + mul255(ch_r(d), ia), g + mul255(ch_g(d), ia), b + mul255(ch_b(d), ia), 255);
				shd = (uint8_t)mul255(shd, ia);
				break;
			}
			case blend::shadow:
				shd = (uint8_t)(shd + mul255(a, 255 - shd));
				break;
			case blend::brighten: {
				uint32_t ia = 255 - a;
				d = rgba(a + mul255(ch_r(d), ia), a + mul255(ch_g(d), ia), a + mul255(ch_b(d), ia), 255);
				break;
			}
			case blend::tinted: {
				uint32_t ia = 255 - a;
				d = rgba(mul255(ch_r(s), tr) + mul255(ch_r(d), ia), mul255(ch_g(s), tg) + mul255(ch_g(d), ia), mul255(ch_b(s), tb) + mul255(ch_b(d), ia), 255);
				shd = (uint8_t)mul255(shd, ia);
				break;
			}
			}
		}
	}
}

// Warp-in (sprite_mapped_frag.glsl): colour from `texture` stretched over the sprite's
// rect, coverage from the sprite's alpha.
inline void draw_sprite_mapped(const canvas& c, const sprite_frame& f, const sprite_frame& texture, int x, int y, bool flipped) {
	int w = f.px.w, h = f.px.h, tw = texture.px.w, th = texture.px.h;
	int x0 = std::max(0, -x), x1 = std::min(w, c.w - x);
	int y0 = std::max(0, -y), y1 = std::min(h, c.h - y);
	if (x0 >= x1 || y0 >= y1 || tw <= 0 || th <= 0) return;
	for (int sy = y0; sy != y1; ++sy) {
		uint32_t* dst = c.px + (size_t)(y + sy) * c.pitch + x;
		uint8_t* sh = c.shadow + (size_t)(y + sy) * c.w + x;
		const uint32_t* src = &f.px.px[(size_t)sy * w];
		const uint32_t* tex = &texture.px.px[(size_t)(sy * th / h) * tw];
		for (int sx = x0; sx != x1; ++sx) {
			int si = flipped ? w - 1 - sx : sx;
			uint32_t a = ch_a(src[si]);
			if (a == 0) continue;
			uint32_t t = tex[si * tw / w];
			// The texture is premultiplied; un-premultiply, then apply the sprite's coverage.
			uint32_t ta = ch_a(t);
			if (ta == 0) continue;
			auto col = [&](uint32_t v) { return std::min(255u, v * 255 / ta); };
			uint32_t ia = 255 - a;
			uint32_t& d = dst[sx];
			d = rgba(mul255(col(ch_r(t)), a) + mul255(ch_r(d), ia), mul255(col(ch_g(t)), a) + mul255(ch_g(d), ia), mul255(col(ch_b(t)), a) + mul255(ch_b(d), ia), 255);
			sh[sx] = (uint8_t)mul255(sh[sx], ia);
		}
	}
}

// Copies an opaque image with its top-left at (x, y).
inline void draw_opaque(const canvas& c, const image& img, int x, int y) {
	int x0 = std::max(0, -x), x1 = std::min(img.w, c.w - x);
	int y0 = std::max(0, -y), y1 = std::min(img.h, c.h - y);
	if (x0 >= x1 || y0 >= y1) return;
	for (int sy = y0; sy != y1; ++sy) {
		std::memcpy(c.px + (size_t)(y + sy) * c.pitch + x + x0, &img.px[(size_t)sy * img.w + x0], (size_t)(x1 - x0) * 4);
	}
}

// A frame's draws, recorded in order and then replayed in parallel horizontal bands: each
// band applies every draw clipped to itself, so overlapping draws compose exactly as if
// drawn one after another. Frames and images must stay alive until run() (graphics keeps
// what it hands out until end_frame()).
struct draw_list {
	struct command {
		enum class kind : uint8_t { sprite, mapped, opaque } what;
		blend mode;
		bool flipped;
		int x, y, w, h;
		uint32_t team, alpha;
		const sprite_frame* frame;  // sprite, mapped
		const sprite_frame* texture;  // mapped
		const image* img;  // opaque
	};
	std::vector<command> commands;

	void clear() { commands.clear(); }

	void sprite(const canvas& c, const sprite_frame& f, int x, int y, bool flipped, blend mode, uint32_t team, uint32_t alpha) {
		if (alpha == 0) return;
		add(c, {command::kind::sprite, mode, flipped, x, y, f.px.w, f.px.h, team, alpha, &f, nullptr, nullptr});
	}
	void mapped(const canvas& c, const sprite_frame& f, const sprite_frame& texture, int x, int y, bool flipped) {
		add(c, {command::kind::mapped, blend::normal, flipped, x, y, f.px.w, f.px.h, 0, 255, &f, &texture, nullptr});
	}
	void opaque(const canvas& c, const image& img, int x, int y) {
		add(c, {command::kind::opaque, blend::normal, false, x, y, img.w, img.h, 0, 255, nullptr, nullptr, &img});
	}

	// Draws everything recorded onto `c`, then clears the list.
	void run(const canvas& c) {
		parallel_ranges((size_t)c.h, 16, [&](size_t from, size_t to) {
			canvas band = c;
			band.px = c.px + from * c.pitch;
			band.shadow = c.shadow + from * c.w;
			band.h = (int)(to - from);
			band.shadow_area = nullptr;  // recorded by add()
			for (auto& d : commands) {
				int y = d.y - (int)from;
				if (y >= band.h || y + d.h <= 0) continue;
				switch (d.what) {
				case command::kind::sprite: draw_sprite(band, *d.frame, d.x, y, d.flipped, d.mode, d.team, d.alpha); break;
				case command::kind::mapped: draw_sprite_mapped(band, *d.frame, *d.texture, d.x, y, d.flipped); break;
				case command::kind::opaque: draw_opaque(band, *d.img, d.x, y); break;
				}
			}
		});
		commands.clear();
	}

private:
	void add(const canvas& c, const command& d) {
		if (d.x >= c.w || d.y >= c.h || d.x + d.w <= 0 || d.y + d.h <= 0 || d.w <= 0 || d.h <= 0) return;
		if (d.what == command::kind::sprite && d.mode == blend::shadow && c.shadow_area) {
			c.shadow_area->add(std::max(d.x, 0), std::max(d.y, 0), std::min(d.x + d.w, c.w), std::min(d.y + d.h, c.h));
		}
		commands.push_back(d);
	}
};

// Darkens shadowed pixels: colour * (1 - 0.5 * coverage), then clears the buffer.
inline void resolve_shadows(const canvas& c) {
	box all{0, 0, c.w, c.h};
	box area = c.shadow_area ? *c.shadow_area : all;
	area = {std::max(area.x0, 0), std::max(area.y0, 0), std::min(area.x1, c.w), std::min(area.y1, c.h)};
	if (c.shadow_area) *c.shadow_area = {};
	if (area.empty()) return;
	parallel_ranges((size_t)(area.y1 - area.y0), 8, [&](size_t from, size_t to) {
		for (int y = area.y0 + (int)from; y != area.y0 + (int)to; ++y) {
			uint32_t* row = c.px + (size_t)y * c.pitch;
			uint8_t* sh = c.shadow + (size_t)y * c.w;
			for (int x = area.x0; x != area.x1; ++x) {
				uint32_t s = sh[x];
				if (!s) continue;
				uint32_t keep = 255 - (s >> 1);
				uint32_t p = row[x];
				row[x] = rgba(mul255(ch_r(p), keep), mul255(ch_g(p), keep), mul255(ch_b(p), keep), 255);
				sh[x] = 0;
			}
		}
	});
}

}

#endif
