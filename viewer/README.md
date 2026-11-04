# OpenBW native replay viewer

A desktop (SDL2) replay viewer built on [OpenBW](https://github.com/OpenBW/openbw)'s
`ui/gfxtest.cpp`, with additions for watching games:

- **Stats overlay**: minerals, gas, supply, army supply and worker supply for each player.
- **Selection panel** laid out like the in-game console. Click a unit to see its wireframe,
  with shields, HP and energy below it. Beside the wireframe are its name, owner and kills,
  and upgrade icons with their levels: armor, weapons, shields. Carriers, Reavers and
  Vultures also show interceptor, scarab or mine counts. Buildings show the training queue
  and research, upgrade, add-on or construction progress bars. Mineral fields and geysers
  show resources left, supply buildings show supply totals, and transports show their cargo.
  Drag-select to get a grid of health-coloured wireframes. As in Brood War, a box that
  catches units leaves buildings out.
- **Graphs** (`g`): a large panel with tabs for income (minerals and gas per minute, a
  15-second moving average), supply, army supply and worker supply over game time.
  Zoom the time axis from the whole game down to the last 15 s with `[` / `]` or the
  mouse wheel over the panel. It covers the part of the game simulated so far, so seek to
  the end to see the whole game.
- **Replay bar** in the bottom-right corner: game timer, speed or pause state, and a seek bar.
- **Zoom** from the keyboard or mouse wheel, and **speed** controls.
- **Sound** through SDL2_mixer, when it is installed.
- **Edge panning**, and a game view capped at **16:9**, with black bars on ultrawide windows.

## Data files

You need the Brood War 1.16.1 MPQs `StarDat.mpq`, `BrooDat.mpq` and `patch_rt.mpq` in one
directory. Remastered stores its data in CASC, not MPQ, so it can't supply them. The viewer
plays replays recorded by 1.16.1-compatible engines: BWAPI 4.x bots and OpenBW.
Replays from Remastered versions newer than 1.18 are not supported.

## Build

Requires CMake, a C++14 compiler and SDL2. SDL2_mixer is optional and enables sound.

```sh
# Arch: pacman -S cmake sdl2 sdl2_mixer    Debian/Ubuntu: apt install cmake libsdl2-dev libsdl2-mixer-dev
cmake -S viewer -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
```

## Run

```sh
./build/openbw-player path/to/game.rep path/to/mpq-dir
```

## Controls

| Key / mouse | Action |
| --- | --- |
| `space` / `p` | pause |
| `=` / `-` (also `a` / `z`) | faster / slower, ×2 per press |
| `ctrl =` / `ctrl -` / `ctrl 0` | zoom in / out / reset |
| mouse wheel | zoom around the cursor |
| arrows, or the mouse at a window edge | scroll |
| `backspace` | rewind about 5 s |
| click or drag the replay bar | seek |
| click / drag / `ctrl`-click / `shift`-click | select (`ctrl`-click selects the same type on screen) |
| `h` | toggle the stats overlay |
| `i` | toggle the selection panel |
| `g` / `esc` | toggle / close the graphs; `tab` or `1`-`4` (or click a tab) to switch |
| `[` / `]`, or the wheel over the graphs | zoom the graphs' time axis out / in |
| `shift tab` | two-colour mode: the two players in red and teal (again to restore) |
| `m` | mute |

Playback starts at 1×, so press `=` a few times.

## Notes

- The overlay font in `ui/hud_font.h` is a bitmap rendering of JetBrains Mono Bold
  (SIL OFL 1.1, see `LICENSE-JetBrainsMono.txt`). Regenerate it with `gen_hud_font.py`.
- `ui/order_names.h` is generated from `bwenums.h` by `gen_order_names.py`.
- Wireframes come from the game's own `unit/wirefram/*.grp` files. Their damage colouring
  (four sections that turn yellow, then red, in a fixed random order per unit) approximates
  Brood War's. It is not a byte-exact copy.
