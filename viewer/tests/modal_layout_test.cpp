// Behaviour tests for the modal card layout (ui/modal_layout.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/modal_layout_test.cpp -o /tmp/modal_layout_test && /tmp/modal_layout_test
#include "modal_layout.h"

#include <cstdio>

using namespace modal_layout;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	// Three buttons stack top to bottom, all as wide as the panel's inner width, never overlapping.
	params p;
	p.title_cols = 11; p.headline_cols = 13; p.button_cols = 14; p.buttons = 3;
	result r = compute(p);
	CHECK(r.buttons.size() == 3);
	for (size_t i = 0; i != 3; ++i) {
		CHECK(r.buttons[i].x0 == p.pad_x && r.buttons[i].x1 == r.w - p.pad_x);
		CHECK(r.buttons[i].y1 - r.buttons[i].y0 == p.button_h);
		if (i) CHECK(r.buttons[i].y0 == r.buttons[i - 1].y1 + p.button_gap);
	}
	CHECK(r.buttons[0].y0 > r.headline_y && r.headline_y > r.title_y);
	CHECK(r.buttons[2].y1 + p.bottom <= r.h);
	CHECK(r.w < 2 * r.h);  // narrow and tall, not a wide row

	// The panel grows for long text and buttons, and keeps a minimum width.
	params q = p; q.title_cols = 60;
	CHECK(compute(q).w > r.w);
	q = p; q.button_cols = 50;
	CHECK(compute(q).w >= (50 + 4) * p.cell_w);
	params tiny; tiny.buttons = 1; tiny.button_cols = 1;
	CHECK(compute(tiny).w >= tiny.min_cols * tiny.cell_w);

	// Result lines make room above the buttons.
	params e = p; e.lines = 2; e.line_cols = 30; e.buttons = 2;
	result re = compute(e);
	CHECK(re.buttons[0].y0 >= re.lines_y + 2 * (p.cell_h + 2));
	CHECK(re.w >= 30 * p.cell_w + 2 * p.pad_x);

	// A width limit is honoured but never collapses the panel to nothing.
	params m = p; m.title_cols = 80; m.max_w = 300;
	CHECK(compute(m).w <= 300);
	m.max_w = 1;
	CHECK(compute(m).w > 0);

	// Zero buttons and degenerate cells do not crash or go negative.
	params z; z.cell_w = 0; z.cell_h = 0;
	result rz = compute(z);
	CHECK(rz.buttons.empty() && rz.w > 0 && rz.h > 0);

	// Hit testing: inside a button, in the gap, outside the card.
	CHECK(button_at(r, r.buttons[1].x0 + 3, r.buttons[1].y0 + 3) == 1);
	CHECK(button_at(r, r.buttons[1].x0 + 3, r.buttons[0].y1 + 1) == -1);
	CHECK(button_at(r, r.buttons[1].x1, r.buttons[1].y0 + 3) == -1);
	CHECK(button_at(r, -5, -5) == -1);

	std::printf(failures ? "%d FAILED\n" : "ok\n", failures);
	return failures != 0;
}
