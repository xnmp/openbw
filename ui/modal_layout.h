#ifndef OPENBW_MODAL_LAYOUT_H
#define OPENBW_MODAL_LAYOUT_H

// The modal cards (the game menu and the end-of-replay card), laid out like the game's own
// Game Menu (rez/gamemenu.ui.json): a 264-pixel-wide dialog with the title at the top and
// 224 x 28 buttons, 34 pixels apart, the last ("Return to Game") set 12 pixels further
// down. Distances are in the game's 640 x 480 pixels and scaled to the window, as
// Remastered scales its menus. Also the buttons' hotkeys. Pure maths, so it can be tested.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace modal_layout {

struct rect { int x0, y0, x1, y1; };

// The game's measurements, in its own pixels.
constexpr int dialog_w = 264, button_x = 20, button_w = 224, button_h = 28;
constexpr int title_y = 16;       // the title's centre
constexpr int first_row = 36;     // the top of the first line or button
constexpr int line_h = 18, lines_gap = 12;
constexpr int button_step = 34, last_gap = 12, bottom = 10;

// Window pixels per game pixel for a window `window_h` tall: the game's 480 rows fill it,
// never shrinking below the game's own size.
inline double scale_for(int window_h) { return std::max(1.0, window_h / 480.0); }

struct result {
	double k = 1;  // the scale used
	int w = 0, h = 0;
	int title_y = 0;            // the title's centre
	std::vector<int> lines_y;   // each text line's centre
	std::vector<rect> buttons;  // top to bottom; the last is the default
};

inline result compute(int lines, int buttons, double k) {
	result r;
	if (!(k > 0) || !std::isfinite(k)) k = 1;
	lines = std::max(0, lines);
	buttons = std::max(0, buttons);
	r.k = k;
	auto s = [&](double v) { return (int)std::lround(v * k); };
	r.w = s(dialog_w);
	r.title_y = s(title_y);
	for (int i = 0; i != lines; ++i) r.lines_y.push_back(s(first_row + (i + 0.5) * line_h));
	double y = first_row + (lines ? lines * line_h + lines_gap : 0);
	for (int i = 0; i != buttons; ++i) {
		if (i && i + 1 == buttons) y += last_gap;  // the default button stands apart
		r.buttons.push_back(rect{s(button_x), s(y), s(button_x + button_w), s(y + button_h)});
		y += button_step;
	}
	double end = buttons ? y - button_step + button_h : y;
	r.h = s(end + bottom);
	return r;
}

// The index of the button containing (x, y) (the card's own pixels), or -1.
inline int button_at(const result& r, int x, int y) {
	for (size_t i = 0; i != r.buttons.size(); ++i) {
		const rect& b = r.buttons[i];
		if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1) return (int)i;
	}
	return -1;
}

// A button's label and keys. `hot` is the index in `text` of the letter the game shows in
// white (-1: none); `keys` are the lower-case letters that press it.
struct button {
	std::string text;
	int hot = -1;
	std::string keys;
};

// The button a key presses (an ASCII key code, either case), or -1.
inline int button_for_key(const std::vector<button>& bs, int key) {
	if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
	if (key <= 0 || key > 127) return -1;
	for (size_t i = 0; i != bs.size(); ++i)
		if (bs[i].keys.find((char)key) != std::string::npos) return (int)i;
	return -1;
}

}  // namespace modal_layout

#endif
