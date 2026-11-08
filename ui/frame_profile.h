// Optional per-section frame timing, for finding what a frame spends its time on.
// Off unless OPENBW_PROFILE is set (to anything but empty or 0; a number above 1 is the
// reporting period in frames, default 240); then every period the average milliseconds per
// frame of each named section go to stderr. When off, a scope costs one predictable branch.
#pragma once

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace frame_profile {

class profile {
	using clock = std::chrono::steady_clock;
	bool on_;
	int period_ = 240;
	int frames_ = 0;
	struct section { std::string name; double wall_ms = 0, cpu_ms = 0; int calls = 0; };
	std::vector<section> sections_;  // in first-seen order
	// CPU time of the calling thread: unlike wall time it is not inflated by a busy machine
	// (work done on other threads is not counted).
	static double thread_cpu_ms() {
		timespec ts;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
	}
public:
	profile() {
		const char* e = std::getenv("OPENBW_PROFILE");
		on_ = e && *e && *e != '0';
		if (on_) { int n = std::atoi(e); if (n > 1) period_ = n; }
	}
	bool on() const { return on_; }
	void add(const char* name, double wall_ms, double cpu_ms) {
		for (auto& s : sections_) if (s.name == name) { s.wall_ms += wall_ms, s.cpu_ms += cpu_ms, ++s.calls; return; }
		section n;
		n.name = name, n.wall_ms = wall_ms, n.cpu_ms = cpu_ms, n.calls = 1;
		sections_.push_back(n);
	}
	// Call once per frame, after the last section.
	void end_frame() {
		if (!on_ || ++frames_ < period_) return;
		std::string line;
		char buf[96];
		for (auto& s : sections_) {
			// per frame: wall/thread-cpu ms, then how many frames ran the section
			std::snprintf(buf, sizeof(buf), " %s=%.3f/%.3f(n%d)", s.name.c_str(), s.wall_ms / frames_, s.cpu_ms / frames_, s.calls);
			line += buf;
			s.wall_ms = s.cpu_ms = 0, s.calls = 0;
		}
		std::fprintf(stderr, "profile ms/frame (%d frames):%s\n", frames_, line.c_str());
		frames_ = 0;
	}

	class scope {
		profile* p_;
		const char* name_;
		clock::time_point t0_;
		double cpu0_ = 0;
	public:
		scope(profile& p, const char* name) : p_(p.on_ ? &p : nullptr), name_(name) { if (p_) t0_ = clock::now(), cpu0_ = thread_cpu_ms(); }
		~scope() { if (p_) p_->add(name_, std::chrono::duration<double, std::milli>(clock::now() - t0_).count(), thread_cpu_ms() - cpu0_); }
		scope(const scope&) = delete;
		scope& operator=(const scope&) = delete;
	};
};

}
