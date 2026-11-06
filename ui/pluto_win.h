#ifndef OPENBW_UI_PLUTO_WIN_H
#define OPENBW_UI_PLUTO_WIN_H

// The Pluto bot's own win estimate over a game, from its stats file
// (bwapi-data/write/pluto_bandit_<opponent>.txt). Below the build-order stats, that file
// keeps one JSON line per game; the end record carries "win_traj": the model's win
// estimate on a -1..1 scale at 64 points spread evenly over the game (each point taken as
// the middle of its 1/64 of the game). Pure: no game state or file access, so it can be
// tested on its own (viewer/tests/pluto_win_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace pluto_win {

struct record {
	std::string opponent;  // "opp": the other player's name, so the owner is the other player
	int frames = 0;        // game length as the bot saw it
	std::vector<double> traj;  // -1..1
};

namespace detail {
// The value of "key": in a one-line JSON object, as raw text (string contents without the
// quotes, or the number/array text); empty if absent. Enough for the bot's flat records.
inline std::string field(const std::string& line, const std::string& key) {
	std::string pat = "\"" + key + "\":";
	size_t p = line.find(pat);
	if (p == std::string::npos) return {};
	p += pat.size();
	while (p < line.size() && line[p] == ' ') ++p;
	if (p >= line.size()) return {};
	if (line[p] == '"') {
		size_t e = line.find('"', p + 1);
		return e == std::string::npos ? std::string() : line.substr(p + 1, e - p - 1);
	}
	if (line[p] == '[') {
		size_t e = line.find(']', p);
		return e == std::string::npos ? std::string() : line.substr(p, e - p + 1);
	}
	size_t e = line.find_first_of(",}", p);
	return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
}

// Every well-formed end record in the file's text, in file order.
inline std::vector<record> parse(const std::string& text) {
	std::vector<record> out;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t eol = text.find('\n', pos);
		std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		pos = eol == std::string::npos ? text.size() : eol + 1;
		static const std::string end_tag = "{\"t\":\"end\"";
		if (line.compare(0, end_tag.size(), end_tag) != 0) continue;
		record r;
		r.opponent = detail::field(line, "opp");
		std::string frames = detail::field(line, "frames"), traj = detail::field(line, "win_traj");
		if (r.opponent.empty() || frames.empty() || traj.size() < 2) continue;
		r.frames = std::atoi(frames.c_str());
		bool ok = r.frames > 0;
		const char* s = traj.c_str() + 1;
		while (ok && *s && *s != ']') {
			char* end = nullptr;
			double v = std::strtod(s, &end);
			if (end == s || !std::isfinite(v)) { ok = false; break; }
			r.traj.push_back(std::max(-1.0, std::min(1.0, v)));
			s = end;
			while (*s == ',' || *s == ' ') ++s;
		}
		if (ok && !r.traj.empty()) out.push_back(std::move(r));
	}
	return out;
}

// The record for a replay of `end_frame` frames against `opponent`: the last one whose
// length is within `tolerance` frames (the bot leaves or ends a few frames off the replay),
// else nullptr. A stats file accumulates every game against that opponent.
inline const record* match(const std::vector<record>& records, const std::string& opponent, int end_frame, int tolerance = 240) {
	const record* best = nullptr;
	for (auto& r : records) {
		if (r.opponent == opponent && std::abs(r.frames - end_frame) <= tolerance) best = &r;
	}
	return best;
}

// Win chance 0..1 at `frame`: linear between the points' mid-segment frames, flat before
// the first and after the last.
inline double chance_at(const record& r, int frame) {
	size_t n = r.traj.size();
	if (n == 0 || r.frames <= 0) return 0.5;
	double pos = (double)frame * n / r.frames - 0.5;  // in units of points
	double v;
	if (pos <= 0) v = r.traj.front();
	else if (pos >= n - 1) v = r.traj.back();
	else {
		size_t i = (size_t)pos;
		double t = pos - i;
		v = r.traj[i] * (1 - t) + r.traj[i + 1] * t;
	}
	return (v + 1) / 2;
}

// When the game was decided, by the bots' own estimates: the earliest frame from which the
// eventual winner's chance stays at or above `threshold` to the end of the game. Each
// player's record is its own estimate (null if missing); with both, the winner's chance is
// the mean of the two views (its own estimate, and one minus the loser's), so neither bot's
// optimism alone decides it. The winner is the side favoured at the end. -1 if the game was
// never decided that way, or only in its last `min_left` frames (the usual end: the loser
// resigns once its own estimate collapses). Sampled every `step` frames.
struct decision {
	int frame = -1;
	int winner = -1;  // 0: the first record's owner, 1: the second's
};
inline decision decided(const record* a, const record* b, int end_frame, double threshold = 0.9, int min_left = 60 * 24, int step = 24) {
	decision d;
	if ((!a && !b) || end_frame <= 0 || step <= 0) return d;
	auto chance_a = [&](int f) {  // the first player's chance of winning at frame f
		double sum = 0;
		int n = 0;
		if (a) sum += chance_at(*a, f), ++n;
		if (b) sum += 1 - chance_at(*b, f), ++n;
		return sum / n;
	};
	double last = chance_a(end_frame);
	if (last == 0.5) return d;
	int winner = last > 0.5 ? 0 : 1;
	auto winning = [&](int f) { double c = chance_a(f); return (winner == 0 ? c : 1 - c) >= threshold; };
	int from = -1;
	for (int f = end_frame / step * step; f >= 0; f -= step) {
		if (!winning(f)) break;
		from = f;
	}
	if (from < 0 || end_frame - from < min_left) return d;
	d.frame = from;
	d.winner = winner;
	return d;
}

}

#endif
