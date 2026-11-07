// Behaviour tests for the modal card layout and hotkeys (ui/modal_layout.h).
// Build and run: c++ -std=gnu++14 -Wall -O1 -fsanitize=address,undefined -Iui viewer/tests/modal_layout_test.cpp -o /tmp/modal_layout_test && /tmp/modal_layout_test
#include "modal_layout.h"

#include <cstdio>
#include <limits>

using namespace modal_layout;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	// At the game's own size the game menu has the game's measurements: Options at y 36,
	// the next 34 below, Return to Game 12 further apart, all 224 x 28 at x 20.
	result r = compute(0, 4, 1.0);
	CHECK(r.w == 264 && r.buttons.size() == 4);
	CHECK(r.buttons[0].x0 == 20 && r.buttons[0].x1 == 244 && r.buttons[0].y0 == 36 && r.buttons[0].y1 == 64);
	CHECK(r.buttons[1].y0 == 70 && r.buttons[2].y0 == 104);
	CHECK(r.buttons[3].y0 == 104 + 34 + 12);
	CHECK(r.h == r.buttons[3].y1 + 10);
	CHECK(r.title_y < r.buttons[0].y0);

	// Result lines sit between the title and the buttons, without overlapping either.
	result e = compute(2, 2, 1.0);
	CHECK(e.lines_y.size() == 2 && e.buttons.size() == 2);
	CHECK(e.lines_y[0] > e.title_y && e.lines_y[1] > e.lines_y[0]);
	CHECK(e.lines_y[1] + line_h / 2 <= e.buttons[0].y0);
	CHECK(e.buttons[1].y0 - e.buttons[0].y1 == button_step - button_h + last_gap);

	// Scaling with the window: proportions kept, nothing below the game's own size.
	CHECK(scale_for(480) == 1.0 && scale_for(960) == 2.0 && scale_for(300) == 1.0 && scale_for(0) == 1.0);
	result big = compute(0, 4, 2.0);
	CHECK(big.w == 528 && big.buttons[3].y1 == 2 * r.buttons[3].y1 && big.h == 2 * r.h);
	result odd = compute(1, 3, 800 / 480.0);
	for (size_t i = 1; i < odd.buttons.size(); ++i) CHECK(odd.buttons[i].y0 > odd.buttons[i - 1].y1);
	CHECK(odd.buttons.back().y1 < odd.h);

	// One button: no gap above it. No buttons or lines: just the title.
	result one = compute(0, 1, 1.0);
	CHECK(one.buttons.size() == 1 && one.buttons[0].y0 == 36);
	result none = compute(0, 0, 1.0);
	CHECK(none.buttons.empty() && none.h > none.title_y);
	// Malformed input: negative counts and nonsensical scales fall back to safe values.
	result neg = compute(-3, -1, 1.0);
	CHECK(neg.buttons.empty() && neg.lines_y.empty() && neg.h == none.h);
	CHECK(compute(0, 2, 0).w == 264 && compute(0, 2, -5).w == 264);
	CHECK(compute(0, 2, std::numeric_limits<double>::quiet_NaN()).w == 264);
	CHECK(compute(0, 2, std::numeric_limits<double>::infinity()).w == 264);

	// Hit testing in the card's pixels: inside a button, on its edges, in the gaps.
	CHECK(button_at(r, 21, 37) == 0 && button_at(r, 243, 63) == 0);
	CHECK(button_at(r, 244, 40) == -1 && button_at(r, 30, 64) == -1 && button_at(r, 30, 66) == -1);
	CHECK(button_at(r, 100, r.buttons[3].y0) == 3);
	CHECK(button_at(r, -1, -1) == -1);

	// Hotkeys: either case, several keys per button, unknown and out-of-range keys ignored.
	std::vector<button> end = {{"Watch Again", 0, "w"}, {"Exit Replay", 1, "xe"}};
	CHECK(button_for_key(end, 'w') == 0 && button_for_key(end, 'W') == 0);
	CHECK(button_for_key(end, 'x') == 1 && button_for_key(end, 'e') == 1 && button_for_key(end, 'E') == 1);
	CHECK(button_for_key(end, 'q') == -1 && button_for_key(end, 27) == -1 && button_for_key(end, 0) == -1);
	CHECK(button_for_key(end, -1) == -1 && button_for_key(end, 0x40000043) == -1 && button_for_key(end, 'x' + 256) == -1);
	CHECK(button_for_key({}, 'w') == -1);
	std::vector<button> keyless = {{"Return to Game (Esc)", -1, ""}};
	CHECK(button_for_key(keyless, 'r') == -1);

	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("modal_layout: all passed\n");
	return 0;
}
