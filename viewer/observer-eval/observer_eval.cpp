// Headless auto-observer evaluation: plays a replay at a simulated playback speed,
// calling observer_step at 60 Hz of simulated wall clock, and logs the camera (with the
// units in view) and ground-truth damage as JSON lines for metrics.py.
//
//   openbw-observer-eval <replay.rep> <mpq dir> <speed> <out.jsonl>
//
// It opens an (unrendered) SDL window, so run it on a virtual display, e.g. xvfb-run.
#define main openbw_gfxtest_main
#include "gfxtest.cpp"
#undef main
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

int main(int argc, char** argv) {
	using namespace bwgame;
	if (argc < 5) { std::fprintf(stderr, "usage\n"); return 2; }
	auto load_data_file = data_loading::data_files_directory(argv[2]);
	game_player player(load_data_file);
	main_t m(std::move(player));
	auto& ui = m.ui;
	ui.load_all_image_data(load_data_file);
	ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) { load_data_file(data, std::move(filename)); };
	ui.init();
	ui.load_replay_file(argv[1]);
	ui.wnd.create("eval", 0, 0, 1280, 800);
	ui.resize(1280, 800);
	double speed = std::atof(argv[3]);
	FILE* out = std::fopen(argv[4], "w");
	std::fprintf(out, "{\"view\":[%d,%d],\"map\":[%d,%d],\"end\":%d,\"speed\":%g}\n", (int)ui.view_width, (int)ui.view_height,
		(int)ui.game_st.map_width, (int)ui.game_st.map_height, (int)ui.replay_st.end_frame, speed);
	ui.auto_observer = true;
	ui.game_speed = fp8::from_raw((int)(speed * 256));
	using clk = std::chrono::high_resolution_clock;
	clk::time_point now{};
	ui.observer_last_update = now;
	double frame_acc = 0;
	const double tick_s = 1.0 / 60;
	std::unordered_map<uint32_t, int> hp;
	ui.screen_pos = {0, 0};
	ui.observer_last_screen_pos = ui.screen_pos;
	while (!ui.is_done()) {
		now += std::chrono::microseconds((int64_t)(tick_s * 1e6));
		frame_acc += tick_s * speed * 1000.0 / 42.0;
		while (frame_acc >= 1 && !ui.is_done()) {
			frame_acc -= 1;
			ui.replay_functions::next_frame();
			ui.replay_frame = ui.st.current_frame;
			for (int i = 0; i != 8; ++i) for (unit_t* u : ptr(ui.st.player_units[i])) {
				uint32_t id = ui.get_unit_id(u).raw_value;
				int h = ui.unit_hp_total(u);
				auto it = hp.find(id);
				if (it != hp.end() && h < it->second && !ui.u_hallucination(u))
					std::fprintf(out, "{\"d\":%d,\"x\":%d,\"y\":%d,\"a\":%d}\n", ui.st.current_frame, (int)u->sprite->position.x, (int)u->sprite->position.y, it->second - h);
				hp[id] = h;
			}
		}
		ui.observer_step(now, false);
		auto& sp = ui.screen_pos;
		if (sp.y + (int)ui.view_height > (int)ui.game_st.map_height) sp.y = ui.game_st.map_height - ui.view_height;
		if (sp.y < 0) sp.y = 0;
		if (sp.x + (int)ui.view_width > (int)ui.game_st.map_width) sp.x = ui.game_st.map_width - ui.view_width;
		if (sp.x < 0) sp.x = 0;
		ui.observer_last_screen_pos = sp;
		int n_units = 0, n_army = 0;
		for (int i = 0; i != 8; ++i) for (unit_t* u : ptr(ui.st.player_units[i])) {
			auto p = u->sprite->position;
			if (p.x < sp.x || p.y < sp.y || p.x >= sp.x + (int)ui.view_width || p.y >= sp.y + (int)ui.view_height) continue;
			++n_units;
			if (!ui.ut_building(u) && !ui.ut_worker(u) && u->unit_type->supply_required.raw_value > 0) ++n_army;
		}
		std::fprintf(out, "{\"t\":%.4f,\"f\":%d,\"x\":%d,\"y\":%d,\"n\":%d,\"na\":%d,\"tx\":%d,\"ty\":%d,\"sw\":%lld}\n", std::chrono::duration<double>(now.time_since_epoch()).count(), ui.st.current_frame, sp.x, sp.y, n_units, n_army, ui.observer_target.x, ui.observer_target.y, (long long)ui.observer_last_switch.time_since_epoch().count());
	}
	std::fclose(out);
	return 0;
}
