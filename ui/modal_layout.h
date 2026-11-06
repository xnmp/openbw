#ifndef OPENBW_MODAL_LAYOUT_H
#define OPENBW_MODAL_LAYOUT_H

// Geometry of the modal cards (the game menu and the end-of-replay card): a narrow panel with a
// title, an optional headline and result lines, then the buttons stacked top to bottom at the
// panel's full inner width, as in the game's own menu. Pixel maths only, so it can be tested.

#include <algorithm>
#include <vector>

namespace modal_layout {

struct rect { int x0, y0, x1, y1; };

struct params {
	int cell_w = 10, cell_h = 22;  // the text cell
	int pad_x = 24;                // panel edge to buttons and text
	int top = 22, bottom = 22;     // rows the panel's frame takes at the top and bottom
	int button_h = 44, button_gap = 8;
	int title_cols = 0, headline_cols = 0;  // text widths in cells (0: no such line)
	int line_cols = 0;                      // the widest result line, in cells
	int lines = 0;
	int button_cols = 0;                    // the longest button label, in cells
	int min_cols = 24;                      // the panel's inner width is at least this many cells
	int buttons = 0;
	int max_w = 0;                          // 0: no limit; else the panel is no wider
};

struct result {
	int w = 0, h = 0;
	int title_y = 0, headline_y = 0, lines_y = 0;  // top of each block (lines_y: the first line)
	std::vector<rect> buttons;
};

inline result compute(const params& p) {
	result r;
	const int cw = std::max(1, p.cell_w), ch = std::max(1, p.cell_h);
	int cols = std::max({p.min_cols, p.title_cols, p.headline_cols, p.line_cols, p.button_cols + 4});
	int inner = cols * cw;
	if (p.max_w > 0) inner = std::min(inner, std::max(cw, p.max_w - 2 * p.pad_x));
	r.w = inner + 2 * p.pad_x;
	int y = p.top + ch / 2;
	r.title_y = y;
	y += ch + ch / 2;
	if (p.headline_cols > 0) {
		r.headline_y = y;
		y += ch + ch / 2;
	}
	r.lines_y = y;
	if (p.lines > 0) y += p.lines * (ch + 2) + ch / 2;
	for (int i = 0; i < p.buttons; ++i) {
		r.buttons.push_back(rect{p.pad_x, y, p.pad_x + inner, y + p.button_h});
		y += p.button_h + (i + 1 < p.buttons ? p.button_gap : 0);
	}
	r.h = y + p.bottom + ch / 2;
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

}  // namespace modal_layout

#endif
