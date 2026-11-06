#ifndef OPENBW_SCR_CASC_H
#define OPENBW_SCR_CASC_H

// Read-only access to a local StarCraft: Remastered CASC store (the "Data" directory of an
// installed game), enough to read the files the remastered renderer needs.
//
// Lookup chain: file name -> content key (root file) -> encoding key (encoding file) ->
// archive/offset (Data/data/*.idx) -> BLTE blob in Data/data/data.NNN -> file bytes.
// StarCraft 1's root is plain text, one "name|ckey-hex" line per file. Only unencrypted
// BLTE chunk modes (N, Z, F) are supported; nothing the renderer reads is encrypted.
//
// Depends on zlib. Reads may run on several threads at once.

#include "scr_parallel.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <io.h>  // the CRT's _findfirst: <windows.h> would leak macros (near, far, ERROR) into includers
#else
#include <dirent.h>
#endif

namespace scr {

struct casc_storage {
	using bytes = std::vector<uint8_t>;

	// Opens the install at `install_dir` (the folder holding .build.info). Returns false and
	// sets `error` if it is not a readable CASC install.
	bool open(const std::string& install_dir, std::string& error) {
		data_dir = install_dir + "/Data";
		std::string build_key;
		if (!active_build_key(install_dir + "/.build.info", build_key)) {
			error = "no active build in " + install_dir + "/.build.info";
			return false;
		}
		bytes config;
		std::string config_path = data_dir + "/config/" + build_key.substr(0, 2) + "/" + build_key.substr(2, 2) + "/" + build_key;
		if (!read_whole_file(config_path, config)) {
			error = "missing build config " + config_path;
			return false;
		}
		auto cfg = parse_config(std::string(config.begin(), config.end()));
		if (cfg["encoding"].size() < 2 || cfg["root"].empty()) {
			error = "build config lacks encoding/root";
			return false;
		}
		if (!load_indices()) {
			error = "no CASC index files in " + data_dir + "/data";
			return false;
		}
		bytes encoding;
		if (!read_ekey(hex_key(cfg["encoding"][1]), encoding) || !parse_encoding(encoding)) {
			error = "unreadable encoding file";
			return false;
		}
		bytes root;
		if (!read_ckey(hex_key(cfg["root"][0]), root)) {
			error = "unreadable root file";
			return false;
		}
		parse_root(root);
		return true;
	}

	// Names are matched case-insensitively ("TileSet/Jungle.dds.vr4" vs "TileSet/badlands...").
	bool has(const std::string& name) const {
		auto i = root.find(lower(name));
		if (i == root.end()) return false;
		auto e = ckey_to_ekey.find(i->second);
		return e != ckey_to_ekey.end() && index.count(index_key(e->second));
	}

	bool read(const std::string& name, bytes& out) const {
		auto i = root.find(lower(name));
		return i != root.end() && read_ckey(i->second, out);
	}

private:
	using key16 = std::array<uint8_t, 16>;
	using key9 = std::array<uint8_t, 9>;
	struct key16_hash {
		size_t operator()(const key16& k) const {
			uint64_t v;
			std::memcpy(&v, k.data(), 8);
			return (size_t)v;
		}
	};
	struct key9_hash {
		size_t operator()(const key9& k) const {
			uint64_t v;
			std::memcpy(&v, k.data(), 8);
			return (size_t)v;
		}
	};
	struct location {
		uint32_t archive;
		uint32_t offset;
		uint32_t size;
	};
	struct file_closer {
		void operator()(FILE* f) const { if (f) std::fclose(f); }
	};

	std::string data_dir;
	std::unordered_map<key9, location, key9_hash> index;
	std::unordered_map<key16, key16, key16_hash> ckey_to_ekey;
	std::unordered_map<std::string, key16> root;
	mutable std::map<uint32_t, std::unique_ptr<FILE, file_closer>> archives;
	mutable std::mutex archives_mutex;  // guards `archives` and their file positions

	static bool read_whole_file(const std::string& path, bytes& out) {
		std::unique_ptr<FILE, file_closer> f(std::fopen(path.c_str(), "rb"));
		if (!f) return false;
		std::fseek(f.get(), 0, SEEK_END);
		long n = std::ftell(f.get());
		if (n < 0) return false;
		std::fseek(f.get(), 0, SEEK_SET);
		out.resize((size_t)n);
		return n == 0 || std::fread(out.data(), (size_t)n, 1, f.get()) == 1;
	}

	static std::string lower(std::string s) {
		for (char& c : s) {
			if (c == '\\') c = '/';
			else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		}
		return s;
	}

	static std::vector<std::string> split(const std::string& s, char sep) {
		std::vector<std::string> r;
		size_t start = 0;
		while (true) {
			size_t p = s.find(sep, start);
			r.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
			if (p == std::string::npos) return r;
			start = p + 1;
		}
	}

	static std::vector<std::string> lines(const std::string& s) {
		std::vector<std::string> r;
		for (auto& l : split(s, '\n')) {
			std::string t = l;
			if (!t.empty() && t.back() == '\r') t.pop_back();
			r.push_back(t);
		}
		return r;
	}

	// .build.info is a pipe-separated table whose header names columns "Name!TYPE:size".
	static bool active_build_key(const std::string& path, std::string& key) {
		bytes raw;
		if (!read_whole_file(path, raw)) return false;
		auto ls = lines(std::string(raw.begin(), raw.end()));
		if (ls.empty()) return false;
		auto header = split(ls[0], '|');
		int active_col = -1, key_col = -1;
		for (size_t i = 0; i != header.size(); ++i) {
			auto name = header[i].substr(0, header[i].find('!'));
			if (name == "Active") active_col = (int)i;
			if (name == "Build Key") key_col = (int)i;
		}
		if (active_col < 0 || key_col < 0) return false;
		for (size_t i = 1; i < ls.size(); ++i) {
			auto row = split(ls[i], '|');
			if ((int)row.size() > std::max(active_col, key_col) && row[active_col] == "1" && row[key_col].size() == 32) {
				key = row[key_col];
				return true;
			}
		}
		return false;
	}

	static std::map<std::string, std::vector<std::string>> parse_config(const std::string& text) {
		std::map<std::string, std::vector<std::string>> r;
		for (auto& l : lines(text)) {
			if (l.empty() || l[0] == '#') continue;
			auto eq = l.find('=');
			if (eq == std::string::npos) continue;
			auto trim = [](std::string s) {
				while (!s.empty() && s.back() == ' ') s.pop_back();
				while (!s.empty() && s.front() == ' ') s.erase(s.begin());
				return s;
			};
			std::vector<std::string> values;
			for (auto& v : split(trim(l.substr(eq + 1)), ' ')) if (!v.empty()) values.push_back(v);
			r[trim(l.substr(0, eq))] = values;
		}
		return r;
	}

	static int hex_digit(char c) {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	static bool parse_hex16(const char* s, key16& k) {
		for (size_t i = 0; i != 16; ++i) {
			int hi = hex_digit(s[i * 2]), lo = hex_digit(s[i * 2 + 1]);
			if (hi < 0 || lo < 0) return false;
			k[i] = (uint8_t)(hi << 4 | lo);
		}
		return true;
	}

	static key16 hex_key(const std::string& s) {
		key16 k{};
		if (s.size() == 32) parse_hex16(s.data(), k);
		return k;
	}

	static key9 index_key(const key16& k) {
		key9 r;
		std::memcpy(r.data(), k.data(), 9);
		return r;
	}

	static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
	static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
	static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

	static std::vector<std::string> list_directory(const std::string& dir) {
		std::vector<std::string> names;
#ifdef _WIN32
		_finddata_t fd;
		intptr_t h = _findfirst((dir + "\\*").c_str(), &fd);
		if (h == -1) return names;
		do names.push_back(fd.name); while (_findnext(h, &fd) == 0);
		_findclose(h);
#else
		DIR* d = opendir(dir.c_str());
		if (!d) return names;
		while (auto* e = readdir(d)) names.push_back(e->d_name);
		closedir(d);
#endif
		return names;
	}

	// Index version 7: for each of the 16 buckets the newest "BBVVVVVVVV.idx" wins.
	bool load_indices() {
		std::string dir = data_dir + "/data";
		std::map<int, std::pair<unsigned long, std::string>> newest;
		for (auto& n : list_directory(dir)) {
			if (n.size() != 14 || n.substr(10) != ".idx") continue;
			int bucket = hex_digit(n[0]) * 16 + hex_digit(n[1]);
			unsigned long version = std::strtoul(n.substr(2, 8).c_str(), nullptr, 16);
			if (bucket < 0) continue;
			auto i = newest.find(bucket);
			if (i == newest.end() || version >= i->second.first) newest[bucket] = {version, n};
		}
		for (auto& v : newest) {
			bytes raw;
			if (!read_whole_file(dir + "/" + v.second.second, raw) || raw.size() < 40) continue;
			uint32_t header_size = le32(&raw[0]);
			const uint8_t* h = &raw[8];
			uint16_t version = (uint16_t)(h[0] | h[1] << 8);
			uint8_t size_len = h[4], offset_len = h[5], key_len = h[6], offset_bits = h[7];
			if (version != 7 || size_len != 4 || offset_len != 5 || key_len != 9) continue;
			size_t pos = (8 + header_size + 15) & ~(size_t)15;
			if (pos + 8 > raw.size()) continue;
			uint32_t entries_size = le32(&raw[pos]);
			pos += 8;
			const size_t entry = 18;
			size_t end = std::min(raw.size(), pos + entries_size);
			uint64_t mask = ((uint64_t)1 << offset_bits) - 1;
			for (size_t p = pos; p + entry <= end; p += entry) {
				key9 k;
				std::memcpy(k.data(), &raw[p], 9);
				uint64_t so = 0;
				for (size_t i = 0; i != 5; ++i) so = so << 8 | raw[p + 9 + i];
				location loc{(uint32_t)(so >> offset_bits), (uint32_t)(so & mask), le32(&raw[p + 14])};
				index.emplace(k, loc);
			}
		}
		return !index.empty();
	}

	// Callers hold archives_mutex.
	FILE* archive(uint32_t n) const {
		auto& f = archives[n];
		if (!f) {
			char name[32];
			std::snprintf(name, sizeof(name), "/data/data.%03u", n);
			f.reset(std::fopen((data_dir + name).c_str(), "rb"));
		}
		return f.get();
	}

	bool read_ekey(const key16& ekey, bytes& out) const {
		auto i = index.find(index_key(ekey));
		if (i == index.end()) return false;
		const size_t blob_header = 30;  // reversed hash, size, flags, checksums
		if (i->second.size < blob_header) return false;
		bytes blob(i->second.size);
		{
			std::lock_guard<std::mutex> lock(archives_mutex);
			FILE* f = archive(i->second.archive);
			if (!f) return false;
			if (std::fseek(f, (long)i->second.offset, SEEK_SET) != 0) return false;
			if (std::fread(blob.data(), blob.size(), 1, f) != 1) return false;
		}
		out.clear();
		return blte_decode(blob.data() + blob_header, blob.size() - blob_header, out, 0, max_decoded_size());
	}

	bool read_ckey(const key16& ckey, bytes& out) const {
		auto i = ckey_to_ekey.find(ckey);
		return i != ckey_to_ekey.end() && read_ekey(i->second, out);
	}

	// No file in the store comes near this; it bounds what a corrupt stream can allocate.
	static size_t max_decoded_size() { return (size_t)1 << 30; }

	struct blte_chunk {
		const uint8_t* data;
		size_t size, decoded_size;
	};

	// Appends the decoded BLTE stream to `out`. A stream with a chunk table must decode each
	// chunk to exactly the size the table gives; its chunks are decoded in parallel (they are
	// independent 64 KB zlib streams in practice), straight into place.
	// At most `limit` bytes are appended; a stream that would exceed it fails.
	static bool blte_decode(const uint8_t* b, size_t n, bytes& out, int depth, size_t limit) {
		if (depth > 2 || n < 8 || std::memcmp(b, "BLTE", 4) != 0) return false;
		uint32_t header_size = be32(b + 4);
		if (header_size == 0) {
			// One chunk of unknown decoded size.
			if (n < 9) return n == 8;
			const uint8_t* d = b + 9;
			size_t dn = n - 9;
			switch (b[8]) {
			case 'N':
				if (dn > limit) return false;
				out.insert(out.end(), d, d + dn);
				return true;
			case 'Z': return inflate_append(d, dn, out, out.size() + limit);
			case 'F': return blte_decode(d, dn, out, depth + 1, limit);
			default: return false;
			}
		}
		if (n < 12 || header_size > n) return false;
		uint32_t count = (uint32_t)b[9] << 16 | b[10] << 8 | b[11];
		if (12 + (size_t)count * 24 > header_size) return false;
		std::vector<blte_chunk> chunks;
		std::vector<size_t> offsets;
		size_t pos = header_size, total = 0;
		for (uint32_t i = 0; i != count; ++i) {
			uint32_t size = be32(b + 12 + i * 24), decoded = be32(b + 16 + i * 24);
			if (pos + size > n || !plausible_chunk(b + pos, size, decoded)) return false;
			chunks.push_back({b + pos, size, decoded});
			offsets.push_back(total);
			pos += size;
			total += decoded;
			if (total > limit) return false;
		}
		size_t base = out.size();
		out.resize(base + total);
		std::atomic<bool> ok{true};
		auto decode = [&](size_t i) {
			if (ok && !decode_chunk(chunks[i], out.data() + base + offsets[i], depth)) ok = false;
		};
		if (depth == 0) parallel_for(chunks.size(), 16, decode);
		else for (size_t i = 0; i != chunks.size(); ++i) decode(i);
		if (!ok) out.resize(base);
		return ok;
	}

	// Whether `size` encoded bytes could decode to `decoded`, checked before anything is
	// allocated for them: 'N' is stored as is, and deflate expands at most 1032:1 (a nested
	// stream adds a little for its own headers).
	static bool plausible_chunk(const uint8_t* data, size_t size, size_t decoded) {
		if (size == 0) return decoded == 0;
		switch (data[0]) {
		case 'N': return decoded == size - 1;
		case 'Z':
		case 'F': return decoded <= size * 1100 + 1024;
		default: return false;
		}
	}

	// Decodes one table chunk into exactly c.decoded_size bytes at `dst`. Does not throw.
	static bool decode_chunk(const blte_chunk& c, uint8_t* dst, int depth) {
		if (c.size == 0) return c.decoded_size == 0;
		const uint8_t* d = c.data + 1;
		size_t dn = c.size - 1;
		switch (c.data[0]) {
		case 'N':
			if (dn != c.decoded_size) return false;
			if (dn) std::memcpy(dst, d, dn);
			return true;
		case 'Z': return inflate_exact(d, dn, dst, c.decoded_size);
		case 'F':
			try {
				bytes inner;
				if (!blte_decode(d, dn, inner, depth + 1, c.decoded_size) || inner.size() != c.decoded_size) return false;
				if (!inner.empty()) std::memcpy(dst, inner.data(), inner.size());
				return true;
			} catch (...) {
				return false;
			}
		default: return false;
		}
	}

	// Inflates a complete zlib stream that must produce exactly `size` bytes.
	static bool inflate_exact(const uint8_t* d, size_t n, uint8_t* dst, size_t size) {
		if (n > UINT32_MAX || size > UINT32_MAX) return false;
		z_stream z{};
		if (inflateInit(&z) != Z_OK) return false;
		z.next_in = const_cast<Bytef*>(d);
		z.avail_in = (uInt)n;
		uint8_t spare;
		z.next_out = size ? dst : &spare;
		z.avail_out = (uInt)size;
		int r = inflate(&z, Z_FINISH);
		bool ok = r == Z_STREAM_END && z.avail_out == 0;
		inflateEnd(&z);
		return ok;
	}

	// Inflates a zlib stream onto `out`, failing if `out` would grow past `limit` bytes.
	static bool inflate_append(const uint8_t* d, size_t n, bytes& out, size_t limit) {
		z_stream z{};
		if (inflateInit(&z) != Z_OK) return false;
		z.next_in = const_cast<Bytef*>(d);
		z.avail_in = (uInt)n;
		uint8_t buf[1 << 16];
		int r;
		do {
			z.next_out = buf;
			z.avail_out = sizeof(buf);
			r = inflate(&z, Z_NO_FLUSH);
			if (r != Z_OK && r != Z_STREAM_END) {
				inflateEnd(&z);
				return false;
			}
			size_t produced = sizeof(buf) - z.avail_out;
			if (out.size() + produced > limit) {
				inflateEnd(&z);
				return false;
			}
			out.insert(out.end(), buf, buf + produced);
		} while (r != Z_STREAM_END && (z.avail_in != 0 || z.avail_out == 0));
		inflateEnd(&z);
		return r == Z_STREAM_END;
	}

	// Encoding file: header, especs, a page table, then pages of
	// {key count, 40-bit size, ckey, ekey * count} entries.
	bool parse_encoding(const bytes& raw) {
		if (raw.size() < 22 || raw[0] != 'E' || raw[1] != 'N') return false;
		size_t ckey_size = raw[3], ekey_size = raw[4];
		size_t page_kb = be16(&raw[5]);
		uint32_t page_count = be32(&raw[9]);
		uint32_t espec_size = be32(&raw[18]);
		if (ckey_size != 16 || ekey_size != 16) return false;
		size_t pos = 22 + espec_size + (size_t)page_count * 32;
		for (uint32_t page = 0; page != page_count; ++page) {
			size_t end = std::min(raw.size(), pos + page_kb * 1024);
			size_t p = pos;
			while (p + 6 + 16 <= end) {
				size_t keys = raw[p];
				if (keys == 0 || p + 6 + 16 + keys * 16 > end) break;
				key16 ck, ek;
				std::memcpy(ck.data(), &raw[p + 6], 16);
				std::memcpy(ek.data(), &raw[p + 6 + 16], 16);
				ckey_to_ekey.emplace(ck, ek);
				p += 6 + 16 + keys * 16;
			}
			pos += page_kb * 1024;
		}
		return !ckey_to_ekey.empty();
	}

	void parse_root(const bytes& raw) {
		std::string text(raw.begin(), raw.end());
		for (auto& l : lines(text)) {
			auto bar = l.rfind('|');
			if (bar == std::string::npos || l.size() - bar - 1 != 32) continue;
			key16 k;
			if (!parse_hex16(l.data() + bar + 1, k)) continue;
			root.emplace(lower(l.substr(0, bar)), k);
		}
	}
};

}

#endif
