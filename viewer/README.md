# OpenBW native replay viewer

A desktop (SDL2) replay viewer built on [OpenBW](https://github.com/OpenBW/openbw)'s
`ui/gfxtest.cpp`, with additions for watching games:

- **Stats overlay**: minerals, gas, supply, army supply and worker supply for each player
  (on by default; turn it off in the `?` menu).
- **Selection panel** laid out like the in-game console. Click a unit to see its wireframe,
  with shields, HP and energy below it. Beside the wireframe are its name, owner and kills,
  and upgrade icons with their levels: armor, shields, weapons (Brood War's order). Carriers, Reavers and
  Vultures also show interceptor, scarab or mine counts. Buildings show the training queue
  and research, upgrade, add-on or construction progress bars. Mineral fields and geysers
  show resources left, supply buildings show supply totals, and transports show their cargo.
  Drag-select to get a grid of health-coloured wireframes. As in Brood War, a box that
  catches units leaves buildings out. Click a wireframe in the grid to select just that unit,
  `shift`-click to drop it, `ctrl`-click to keep only units of its type.
- **Graphs** (`g`): a large panel with tabs for income (minerals and gas per minute, a
  smoothed over about 15 s), supply, army supply, workers and units lost over game time.
  `d` switches to the first derivative (change per minute). `f` plots the first player
  minus the second, coloured by whoever is ahead. `m` pins the graphs minimized in the
  top-right corner of the view, where they keep updating; `g` or a click then maximizes /
  minimizes them instead of closing them, and `m` on the full panel unpins. Graph keys act
  only on the full panel.
  `r` switches supply, army and losses between supply and resource value (minerals + gas,
  with Zergling/Scourge pairs, morphs and Archons valued correctly).
  It shows the last 5 minutes at first; zoom the time axis from the whole game down to
  the last 15 s with `[` / `]` or the mouse wheel over the panel. It covers the part of the game simulated so far, so seek to
  the end to see the whole game.
- **Auto observer** (`o`): the camera goes where the action is. A 128 px interest map,
  smoothed over game time, scores recent damage, armies that are fighting or about to
  meet, and army size; in quiet spells, scouting workers and new buildings, so the
  camera tours the map rather than sitting still. A shot lasts at least a couple of
  seconds and a new scene must be clearly better, so the camera does not dart about.
  It cuts to distant scenes like a human observer and pans smoothly to nearby ones,
  with timing that adapts to the playback speed. Moving the camera yourself pauses it
  for a few seconds.
- **Fog of war** (`f`, with the graphs closed): the combined vision of all players, drawn
  as Brood War does (explored ground dimmed, unexplored black, soft edges), on the
  minimap too.
- **Settings menu** (`?`): checkboxes for every toggle (overlay, selection panel, health
  bars, fog, two-colour mode, auto observer, mute, graph modes). Choices, whether made
  there or with the keys, are saved to `$XDG_CONFIG_HOME/openbw-viewer/settings`
  (`~/.config/...`; `%APPDATA%\openbw-viewer\settings` on Windows) and restored next time.
- **Replay bar** in the bottom-right corner: game timer, speed or pause state, and a seek bar.
- **Zoom** from the keyboard or mouse wheel, and **speed** controls.
- **Sound** through SDL2_mixer, when it is installed.
- **Edge panning**, and a game view capped at **16:9**, with black bars on ultrawide windows.
  With bars, the cursor is kept inside the game view while the window has focus (SDL
  2.0.18+ pointer confinement), so edge panning triggers where the view ends.

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
| `?` | settings menu: a checkbox for every toggle below, plus a key summary |
| `space` / `p` | pause |
| `=` / `-` (also `a` / `z`) | faster / slower: ×2 per press from 1/4× to 128×, with 1.5× between 1× and 2× |
| `ctrl =` / `ctrl -` / `ctrl 0` | zoom in / out / reset |
| mouse wheel | zoom around the cursor |
| arrows, or the mouse at the game view's edge | scroll (with the full graph panel open, `←` / `→` switch its tabs) |
| `backspace` | rewind about 5 s |
| click or drag the replay bar | seek |
| click / drag / `ctrl`-click / `shift`-click | select (`ctrl`-click selects the same type on screen) |
| click a wireframe in the selection grid | select that unit (`shift` drop it, `ctrl` keep its type) |
| `b` | health bars under all units, or only selected ones (all by default) |
| `i` | toggle the selection panel |
| `g` / `esc` | toggle / close the graphs (`g` on the minimized panel restores it); `tab`, `←` / `→` or `1`-`5` (or click a tab) to switch |
| `r` | graphs: supply or resource value |
| `d` | graphs: level or rate of change (with the graphs closed, `d` slows playback) |
| `f` | graphs: players' difference or a line each (with the graphs closed, fog of war) |
| `o` | auto observer on / off |
| `[` / `]`, or the wheel over the graphs | zoom the graphs' time axis out / in |
| `shift tab` | two-colour mode: the two players in red and teal (again to restore) |
| `m` | full graph panel: pin minimized in the corner / unpin (otherwise mute) |

Playback starts at 1×, so press `=` a few times.

## Notes

- Kill counts: upstream OpenBW never increments them; this fork credits the killing
  unit (an Interceptor's or Scarab's kills go to its Carrier or Reaver). Kill counts are
  display only, so replays still play back identically.

- The overlay font in `ui/hud_font.h` is a bitmap rendering of JetBrains Mono Bold
  (SIL OFL 1.1, see `LICENSE-JetBrainsMono.txt`). Regenerate it with `gen_hud_font.py`.
- `ui/order_names.h` is generated from `bwenums.h` by `gen_order_names.py`.
- Wireframes come from the game's own `unit/wirefram/*.grp` files. Colours are remapped
  through `game/twire.pcx` by missing health and shields, as the game does. This covers
  the blue shield outline, green/yellow/red hull sections, and the Zerg heat-map
  ramp, using the tables documented on
  [staredit.net](https://staredit.net/topic/18436/). Which hull section changes first
  is an approximation: the per-unit pattern is undocumented.

## Tuning the auto observer

`observer-eval/` measures the auto observer without watching: `openbw-observer-eval` plays a
replay headlessly at a chosen playback speed with the observer on and logs the camera and
every hp loss; `metrics.py` scores how much of the damage was on screen (against the most a
single view could have caught) and how calm the camera was (cuts, short shots, panning,
reversals).

```sh
cmake --build build --target openbw-observer-eval
for s in 2 8 16; do xvfb-run -a ./build/openbw-observer-eval game.rep mpq-dir $s out-x$s.jsonl; done
python3 viewer/observer-eval/metrics.py out-x*.jsonl
```
