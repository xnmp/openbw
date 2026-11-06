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

}

#endif
