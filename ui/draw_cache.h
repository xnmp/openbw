// Redraw-only-when-changed bookkeeping for surfaces that are expensive to build.
//
// A cached surface is described by a key: a plain struct of every input its drawing reads
// (game frame, camera, sizes, settings, selection, ...). Each frame the caller builds the key
// and asks needs_redraw(key); the surface is rebuilt only when the key differs from the one it
// was last built with. Prefer one explicit key over scattered dirty flags: a forgotten input
// then shows up as a missing field in one struct rather than as a missed flag-set somewhere.
#pragma once

#include <cstdlib>

namespace draw_cache {

// Key must be default constructible and equality comparable.
template<typename Key>
class cache {
	Key last_{};
	bool valid_ = false;
public:
	// True when the surface must be rebuilt for `key` (first use, after invalidate(), or a
	// changed key). The key is remembered, so call this only when the rebuild will follow.
	bool needs_redraw(const Key& key) {
		if (valid_ && last_ == key && !disabled()) return false;
		last_ = key;
		valid_ = true;
		return true;
	}
	// Forget the last key: the next needs_redraw() is true (the surface was recreated or its
	// contents were lost).
	void invalidate() { valid_ = false; }
	bool valid() const { return valid_; }
	// OPENBW_NO_DRAW_CACHE=1 redraws every frame, to compare timings and pixels with and without.
	static bool disabled() {
		static const bool off = [] { const char* e = std::getenv("OPENBW_NO_DRAW_CACHE"); return e && *e && *e != '0'; }();
		return off;
	}
};

}
