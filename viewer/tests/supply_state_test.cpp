// Behaviour tests for the HUD's supply colouring (ui/supply_state.h).
// Build and run: cmake --build build --target openbw-supply-state-test && ./build/openbw-supply-state-test
#include "supply_state.h"

#include <cstdio>

using supply_state::pressure;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	// In whole supply: x/48 is used x of 48 (96 halves).
	CHECK(supply_state::of(48 * 2, 96) == pressure::blocked);       // 48/48
	CHECK(supply_state::of(47 * 2 + 1, 96) == pressure::blocked);   // 47.5/48, shown 48/48
	CHECK(supply_state::of(47 * 2, 96) == pressure::nearly_blocked);  // 47/48: one supply left
	CHECK(supply_state::of(46 * 2 + 1, 96) == pressure::nearly_blocked);  // 46.5/48
	CHECK(supply_state::of(46 * 2, 96) == pressure::free);          // 46/48: room for a Dragoon
	CHECK(supply_state::of(10, 400) == pressure::free);             // maxed out at 200: nothing to warn about
	CHECK(supply_state::of(400, 400) == pressure::free);            // 200/200
	CHECK(supply_state::of(100, 90) == pressure::blocked);          // over the cap (supply lost)
	CHECK(supply_state::of(0, 0) == pressure::blocked);             // no supply at all
	CHECK(supply_state::of(-4, 18) == pressure::free);              // malformed input does not crash
	std::printf(failures ? "%d failures\n" : "all passed\n", failures);
	return failures != 0;
}
