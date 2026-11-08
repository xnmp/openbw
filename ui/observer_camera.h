#ifndef OPENBW_OBSERVER_CAMERA_H
#define OPENBW_OBSERVER_CAMERA_H

// The auto observer's two decisions, as pure maths so they can be tested.
//
// follower: where the camera is. A critically damped spring (the standard game-camera
// follower: no overshoot, velocity always continuous) pulls the view centre to the scene's
// centre. Every frame it moves a little, so the picture glides instead of waiting and
// jumping. There is no hard dead zone, which made the camera sit still until the scene had
// drifted a third of a view away and then lurch: errors smaller than `dead0` of a view are
// ignored and the pull fades in smoothly up to `dead1`, so tiny wobbles of the scene's centre
// never reach the picture and nothing starts or stops abruptly. The spring stiffens with the
// distance (a far scene is reached in well under a second) and with the playback speed. Only
// a scene several views away is cut to, as on a minimap click.
//
// shot_director: which scene is shown. It compares the best scene on the map with the one on
// screen and decides when to move on. Hysteresis comes from the ratio a challenger must
// beat the shown scene by, not from long minimum shot lengths: a clearly bigger fight takes
// over almost at once, a similar one never does, and a quiet shot gives way to a fight
// immediately.

#include <algorithm>
#include <cmath>

namespace observer_camera {

struct follower_params {
	double settle_s = 0.6;      // time to settle on a nearby target at normal speed
	double dead0 = 0.015;       // fraction of a view: errors below this are ignored
	double dead1 = 0.08;        // ... and the pull is complete from here
	double max_views_s = 3.0;   // top speed, views per second
	double cut_views = 3.5;     // farther than this (views) the camera cuts
	double dist_boost = 0.5;    // stiffness grows by this much per view of distance (up to 2 views)
};

struct follower {
	double x = 0, y = 0, vx = 0, vy = 0;  // view centre and its velocity, px and px/s

	void place(double px, double py) { x = px; y = py; vx = vy = 0; }

	// Advance by dt real seconds towards (tx, ty). `speed` is the playback speed (1 = normal).
	// Returns true if the camera cut instead of moving.
	bool step(double tx, double ty, double dt, double vw, double vh, double speed, const follower_params& p = {}) {
		dt = std::max(0.0, std::min(dt, 0.25));
		double ex = tx - x, ey = ty - y;
		double nx = ex / vw, ny = ey / vh, m = std::hypot(nx, ny);
		if (m > p.cut_views) { place(tx, ty); return true; }
		// Soft dead zone: scale the error by a smoothstep of its size.
		double g = std::max(0.0, std::min(1.0, (m - p.dead0) / (p.dead1 - p.dead0)));
		g = g * g * (3 - 2 * g);
		ex *= g; ey *= g;
		double omega = 4.0 / p.settle_s;
		omega *= std::pow(std::max(1.0, std::min(speed, 8.0)), 0.5);
		omega *= 1.0 + p.dist_boost * std::min(m, 2.0);
		double decay = std::exp(-omega * dt);
		// Exact critically damped step towards (x + ex, y + ey).
		double c2x = vx - omega * ex, c2y = vy - omega * ey;  // e0 = -ex
		double nex = (-ex + c2x * dt) * decay, ney = (-ey + c2y * dt) * decay;
		vx = (c2x - omega * (-ex + c2x * dt)) * decay;
		vy = (c2y - omega * (-ey + c2y * dt)) * decay;
		double cap = p.max_views_s * std::max(vw, vh);
		double mx = ex + nex, my = ey + ney, step = std::hypot(mx, my);
		if (step > cap * dt && step > 0) { mx *= cap * dt / step; my *= cap * dt / step; }
		x += mx;
		y += my;
		double sp = std::hypot(vx, vy);
		if (sp > cap) { vx *= cap / sp; vy *= cap / sp; }
		return false;
	}
};

// Two followers in series: the first smooths the target's own jumps (a new shot, or the scene's
// centre updating every few frames), the second is the camera. A single spring has a velocity
// that is continuous but an acceleration that steps at the start of every pan, which reads as
// a snap; in series the acceleration also starts from zero. The dead zone sits on the camera.
struct rig {
	follower aim, cam;
	follower_params aim_p, cam_p;
	rig() {
		aim_p.settle_s = 0.4; aim_p.dead0 = 0; aim_p.dead1 = 1e-9;
		cam_p.settle_s = 0.5; cam_p.cut_views = 1e9;
	}
	void place(double px, double py) { aim.place(px, py); cam.place(px, py); }
	// Returns true if the camera cut.
	bool step(double tx, double ty, double dt, double vw, double vh, double speed) {
		if (aim.step(tx, ty, dt, vw, vh, speed, aim_p)) { cam.place(tx, ty); return true; }
		cam.step(aim.x, aim.y, dt, vw, vh, speed, cam_p);
		return false;
	}
	double x() const { return cam.x; }
	double y() const { return cam.y; }
};

struct scene {
	double score = 0;   // total interest
	double action = 0;  // the fight part of it
	double x = 0, y = 0;
};

struct director_params {
	double ratio = 1.3, bias = 0.2;     // a challenger must beat the shown scene by score*ratio+bias
	double min_shot = 0.8;              // seconds; a fight against a fight
	double min_dwarfed = 0.4;           // the shown scene is dwarfed by the challenger
	double min_over = 0.6;              // the shown fight has died down, or was quiet
	double min_tour = 4.0;              // quiet to quiet: do not flicker
	double hold = 0.1;                  // seconds a challenger must keep leading
};

struct shot_director {
	bool has_shot = false;
	double shot_start = 0, peak = 0;
	int challenger_ticks = 0;
	double cx = 0, cy = 0, challenger_since = 0;

	void start_shot(double now, double action) {
		has_shot = true; shot_start = now; peak = action; challenger_ticks = 0;
	}

	// One decision tick. `current` is the shown scene as it is now (tracked to where its action
	// has moved); `best` is the best scene on the map. Returns true when the camera should move
	// to `best` (the caller then calls start_shot).
	bool tick(double now, const scene& current, const scene& best, double vw, double vh, double speed = 1, const director_params& p = {}) {
		// Shot lengths are in real seconds at normal speed and shrink with playback speed (a fight
		// is over sooner in real time).
		double k = 1.0 / std::sqrt(std::max(1.0, std::min(speed, 8.0)));
		peak = std::max(peak, current.action);
		bool elsewhere = std::abs(best.x - current.x) > vw / 2 || std::abs(best.y - current.y) > vh / 2;
		if (!(elsewhere && best.score > current.score * p.ratio + p.bias)) { challenger_ticks = 0; return false; }
		bool same = challenger_ticks > 0 && std::abs(best.x - cx) < vw / 2 && std::abs(best.y - cy) < vh / 2;
		if (!same) challenger_since = now;
		challenger_ticks = same ? challenger_ticks + 1 : 1;
		cx = best.x; cy = best.y;
		bool over = peak > 4 && current.action < peak * 0.25;
		bool quiet_shot = peak < 2;
		bool dwarfed = best.score > current.score * 2.5 + 2;
		bool fight = best.action >= 2;
		double min_shot = !fight ? p.min_tour : dwarfed ? p.min_dwarfed : (over || quiet_shot) ? p.min_over : p.min_shot;
		return challenger_ticks >= 2 && now - challenger_since >= p.hold * k && now - shot_start >= min_shot * k;
	}
};

}  // namespace observer_camera

#endif
