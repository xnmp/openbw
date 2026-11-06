#ifndef NATIVE_WINDOW_H
#define NATIVE_WINDOW_H

#include <cstdint>
#include <memory>
#include <vector>

namespace native_window {
	struct window_impl;
	
	struct event_t {
		enum {
			type_none,
			type_quit,
			type_key_down,
			type_key_up,
			type_resize,
			type_mouse_button_down,
			type_mouse_button_up,
			type_mouse_motion,
			type_mouse_wheel
		};
		int type = type_none;
		int sym = -1;
		int scancode = -1;
		int width = -1;
		int height = -1;
		int button = -1;
		int mouse_x = -1;
		int mouse_y = -1;
		int button_state = 0;
		int mouse_xrel = -1;
		int mouse_yrel = -1;
		int clicks = -1;
		int wheel_y = 0; // type_mouse_wheel: +up / -down; mouse_x/y hold the cursor
	};

	// A cursor image: w x h pixels as r | g << 8 | b << 16 | a << 24 (straight alpha), with
	// the pointer's hotspot at (hot_x, hot_y).
	struct cursor_image {
		int w = 0, h = 0, hot_x = 0, hot_y = 0;
		std::vector<uint32_t> px;
	};

	struct window {
		std::unique_ptr<window_impl> impl;
		window();
		~window();
		window(window&&);
		void destroy();
		bool create(const char* title, int x, int y, int width, int height);
		void get_cursor_pos(int* x, int* y);
		bool has_mouse_focus();
		// Keep the cursor inside this window-pixel rect while the window has focus;
		// w <= 0 releases it. No-op where the backend cannot confine the pointer.
		void confine_cursor(int x, int y, int w, int h);
		bool peek_message(event_t& e);
		bool show_cursor(bool show);
		bool get_key_state(int scancode);
		bool get_mouse_button_state(int button);
		void update_surface();
		// The refresh rate of the display the window is on, in Hz; 0 if unknown.
		int refresh_rate();
		// Before the first frame: present each frame on the display's refresh (through a
		// renderer) instead of copying it to the window as soon as it is drawn. Falls back
		// to the latter where vsync is unavailable; has_vsync() tells which is in use.
		void present_with_vsync(bool on);
		bool has_vsync() const;
		// Replaces the mouse cursor with these frames (an animation; see show_cursor_frame),
		// starting with the first, and returns how many could be made into cursors. None
		// restores the system cursor.
		size_t set_cursor_frames(const std::vector<cursor_image>& frames);
		void show_cursor_frame(size_t index);
		explicit operator bool() const;
	};
}

#endif

