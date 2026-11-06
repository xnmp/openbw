#ifndef OPENBW_SCR_PARALLEL_H
#define OPENBW_SCR_PARALLEL_H

// A minimal fork-join loop for the remastered renderer: decompressing archive chunks,
// decoding and scaling art, and the full-screen passes of each frame (fog, shadows, overlay)
// split into independent items. Workers are started once and sleep between calls, so a
// call costs a few microseconds rather than thread start-up.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace scr {

inline size_t max_worker_threads() {
	return std::max<size_t>(1, std::min<size_t>(8, std::thread::hardware_concurrency()));
}

// True on a thread while it runs parallel_for() items.
inline bool& in_parallel_for() {
	static thread_local bool inside = false;
	return inside;
}

namespace detail {

class worker_pool {
public:
	// One call's items. Each worker that picks up a job holds it until it finds no items
	// left, so a late worker never touches a finished call's function.
	struct job {
		void (*run)(const void* f, size_t i);
		const void* f;
		size_t n;
		std::atomic<size_t> next{0}, done{0};
		std::mutex error_mutex;
		std::exception_ptr error;  // the first exception an item threw
	};

	explicit worker_pool(size_t threads) {
		for (size_t i = 0; i != threads; ++i) {
			try {
				workers.emplace_back([this] { work(); });
			} catch (...) {
				break;
			}
		}
	}
	~worker_pool() {
		{
			std::lock_guard<std::mutex> l(m);
			stopping = true;
		}
		wake.notify_all();
		for (auto& t : workers) t.join();
	}
	size_t threads() const { return workers.size(); }

	// Runs the job's items on the workers and the calling thread; returns when all are done.
	void run(const std::shared_ptr<job>& j) {
		{
			std::lock_guard<std::mutex> l(m);
			current = j;
			++generation;
		}
		wake.notify_all();
		take_items(*j);
		{
			std::unique_lock<std::mutex> l(m);
			finished.wait(l, [&] { return j->done.load() == j->n; });
			if (current == j) current.reset();
		}
		if (j->error) std::rethrow_exception(j->error);
	}

private:
	std::mutex m;
	std::condition_variable wake, finished;
	std::vector<std::thread> workers;
	std::shared_ptr<job> current;
	uint64_t generation = 0;
	bool stopping = false;

	struct inside_scope {
		bool was = in_parallel_for();
		inside_scope() { in_parallel_for() = true; }
		~inside_scope() { in_parallel_for() = was; }
	};

	// Every item is counted done, even one that throws, so run() always returns, and
	// only after no thread can still be calling the job's function.
	void take_items(job& j) {
		inside_scope inside;
		for (size_t i; (i = j.next.fetch_add(1)) < j.n;) {
			try {
				j.run(j.f, i);
			} catch (...) {
				std::lock_guard<std::mutex> l(j.error_mutex);
				if (!j.error) j.error = std::current_exception();
			}
			if (j.done.fetch_add(1) + 1 == j.n) {
				std::lock_guard<std::mutex> l(m);
				finished.notify_all();
			}
		}
	}

	void work() {
		uint64_t seen = 0;
		std::unique_lock<std::mutex> l(m);
		for (;;) {
			wake.wait(l, [&] { return stopping || (generation != seen && current); });
			if (stopping) return;
			seen = generation;
			std::shared_ptr<job> j = current;
			l.unlock();
			take_items(*j);
			l.lock();
		}
	}
};

inline worker_pool& pool() {
	static worker_pool p(max_worker_threads() - 1);
	return p;
}

}

// Calls f(i) for every i in [0, n) on the calling thread and the shared workers (at most
// max_worker_threads() threads in all), and returns when all calls are done. If calls
// throw, the other items still run and the first exception is rethrown at the end. With
// fewer than 2 * min_per_call items, and for calls made from inside `f`, the items run
// serially on the calling thread.
template <typename F>
void parallel_for(size_t n, size_t min_per_call, F f) {
	if (n < 2 * std::max<size_t>(1, min_per_call) || max_worker_threads() == 1 || in_parallel_for()) {
		for (size_t i = 0; i != n; ++i) f(i);
		return;
	}
	auto& p = detail::pool();
	if (p.threads() == 0) {
		for (size_t i = 0; i != n; ++i) f(i);
		return;
	}
	auto j = std::make_shared<detail::worker_pool::job>();
	j->run = [](const void* fp, size_t i) { (*static_cast<const F*>(fp))(i); };
	j->f = &f;
	j->n = n;
	p.run(j);
}

// Splits [0, n) into contiguous ranges of at least `min_range` items and calls f(begin, end)
// for each, in parallel.
template <typename F>
void parallel_ranges(size_t n, size_t min_range, F f) {
	size_t parts = std::min(n / std::max<size_t>(1, min_range), max_worker_threads() * 4);
	if (parts <= 1) {
		if (n) f((size_t)0, n);
		return;
	}
	parallel_for(parts, 1, [&](size_t p) { f(n * p / parts, n * (p + 1) / parts); });
}

}

#endif
