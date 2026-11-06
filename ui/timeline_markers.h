#ifndef OPENBW_UI_TIMELINE_MARKERS_H
#define OPENBW_UI_TIMELINE_MARKERS_H

// Seek-bar markers from a replay's unit losses: the first unit lost, and the big battles.
// Pure: no game state, so it can be tested on its own (viewer/tests/timeline_test.cpp).

#include <algorithm>
#include <array>
#include <vector>

namespace timeline {

// One completed unit dying, as the value graphs count it (see unit_loss_functions).
struct loss {
	int frame = 0;
	int owner = 0;  // player slot 0..7
	int value = 0;  // minerals + gas
	int unit_type = -1;
};

struct battle {
	int start = 0, end = 0;  // frames of the first and last loss in the battle
	std::array<long long, 8> lost{};  // value lost per player slot
	long long total = 0;
};

struct markers {
	bool has_first_loss = false;
	loss first_loss;
	std::vector<battle> battles;  // in time order
};

struct params {
	int bin_frames = 24;      // about one game second
	int window_bins = 20;     // a battle is scored by the value lost within 20 s
	int min_value = 2000;     // and must destroy at least this much
	int gap_bins = 30;        // battles' windows are at least this far apart
	int max_battles = 12;
	int extend_gap_bins = 5;  // a battle runs on while losses come at most 5 s apart
};

// Battles are picked greedily: the 20 s window with the most value lost (the earliest, on a
// tie), then the best window at least gap_bins clear of every chosen one, until none reaches
// min_value. Each battle then extends past its window while losses continue (see
// extend_gap_bins) and spans its first to last loss. Losses outside [0, end_frame] and
// owners outside 0..7 are ignored; the input need not be sorted.
inline markers find_markers(std::vector<loss> losses, int end_frame, const params& p = {}) {
	markers r;
	losses.erase(std::remove_if(losses.begin(), losses.end(), [&](const loss& l) {
		return l.frame < 0 || l.frame > end_frame || l.owner < 0 || l.owner >= 8 || l.value < 0;
	}), losses.end());
	if (losses.empty() || p.bin_frames <= 0 || p.window_bins <= 0) return r;
	std::stable_sort(losses.begin(), losses.end(), [](const loss& a, const loss& b) { return a.frame < b.frame; });
	r.has_first_loss = true;
	r.first_loss = losses.front();

	const int bins = end_frame / p.bin_frames + 1;
	std::vector<long long> per_bin(bins, 0);
	for (auto& l : losses) per_bin[l.frame / p.bin_frames] += l.value;
	// window[i] = value lost in bins [i, i + window_bins)
	std::vector<long long> prefix(bins + 1, 0);
	for (int i = 0; i != bins; ++i) prefix[i + 1] = prefix[i] + per_bin[i];
	auto window = [&](int i) { return prefix[std::min(bins, i + p.window_bins)] - prefix[i]; };

	std::vector<std::pair<int, int>> taken;  // chosen windows' bin ranges [from, to)
	auto clear = [&](int i) {
		for (auto& t : taken) {
			if (i + p.window_bins + p.gap_bins > t.first && i < t.second + p.gap_bins) return false;
		}
		return true;
	};
	while ((int)taken.size() < p.max_battles) {
		int best = -1;
		for (int i = 0; i != bins; ++i) {
			if (clear(i) && (best < 0 || window(i) > window(best))) best = i;
		}
		if (best < 0 || window(best) < p.min_value) break;
		taken.push_back({best, best + p.window_bins});
	}
	std::sort(taken.begin(), taken.end());
	// A fight longer than the window carries on past it: extend each battle while losses
	// keep coming at most extend_gap_bins apart, stopping halfway to its neighbours.
	for (size_t k = 0; k != taken.size(); ++k) {
		int lo = k ? (taken[k - 1].second + taken[k].first) / 2 : 0;
		int hi = k + 1 != taken.size() ? (taken[k].second + taken[k + 1].first) / 2 : bins;
		auto& t = taken[k];
		for (int gap = 0; t.first - gap - 1 >= lo && gap < p.extend_gap_bins;) {
			if (per_bin[t.first - gap - 1]) { t.first -= gap + 1; gap = 0; } else ++gap;
		}
		for (int gap = 0; t.second + gap < hi && gap < p.extend_gap_bins;) {
			if (per_bin[t.second + gap]) { t.second += gap + 1; gap = 0; } else ++gap;
		}
	}

	for (auto& t : taken) {
		battle b;
		b.start = -1;
		for (auto& l : losses) {
			int bin = l.frame / p.bin_frames;
			if (bin < t.first || bin >= t.second) continue;
			if (b.start < 0) b.start = l.frame;
			b.end = l.frame;
			b.lost[l.owner] += l.value;
			b.total += l.value;
		}
		r.battles.push_back(b);
	}
	return r;
}

// The marker at `frame`, within `tolerance` frames (a few pixels of the seek bar): the first
// loss before any battle. seek_frame is `lead` frames before the marker, for click-to-seek.
struct hit {
	enum kind_t { none, first_loss, battle } kind = none;
	size_t battle_index = 0;
	int seek_frame = -1;
};
inline hit marker_at(const markers& m, int frame, int tolerance, int lead) {
	hit h;
	if (m.has_first_loss && frame >= m.first_loss.frame - tolerance && frame <= m.first_loss.frame + tolerance) {
		h.kind = hit::first_loss;
		h.seek_frame = std::max(0, m.first_loss.frame - lead);
		return h;
	}
	for (size_t i = 0; i != m.battles.size(); ++i) {
		const battle& b = m.battles[i];
		if (frame < b.start - tolerance || frame > b.end + tolerance) continue;
		h.kind = hit::battle;
		h.battle_index = i;
		h.seek_frame = std::max(0, b.start - lead);
		return h;
	}
	return h;
}

}

#endif
