# OpenBW native replay viewer

A desktop (SDL2) replay viewer built on [OpenBW](https://github.com/OpenBW/openbw)'s
`ui/gfxtest.cpp`, with additions for watching games:

- **Stats overlay**, top right: a line per player in the style of Brood War's resource bar,
  with its mineral, gas and supply icons for the player's race, then army supply and worker
  count beside the race's Marine / Zergling / Zealot and SCV / Drone / Probe icons. Supply
  turns red when blocked, and a fainter red one supply short of it (47/48). On by default; turn it off in the `?` menu.
- **Selection panel** laid out like the in-game console. Click a unit to see its wireframe,
  with shields, HP and energy below it, and what a Defensive Matrix on it has left to absorb (its icon and the number; hover for the tip). Beside the wireframe are its name, owner and kills,
  and upgrade icons with their levels: armor, shields, weapons (Brood War's order). Hover
  one for its name and numbers, base plus upgrades: armor, shield armor, or a weapon's damage
  (with hits per attack, e.g. a Zealot's 8x2, and an icon for explosive or concussive) and range. Carriers, Reavers and
  Vultures also show interceptor, scarab or mine counts. A second row shows completed research
  that affects the unit (e.g. Metabolic Boost and Adrenal Glands on a Zergling, Stim Packs on a
  Marine); hover an icon for its name. Buildings show the training queue
  and research, upgrade, add-on or construction progress bars. A morphing Lair, Hive,
  Greater Spire or Sunken/Spore Colony shows as the building it is becoming. Mineral fields and geysers
  show resources left, and transports show their cargo.
  Drag-select to get a grid of health-coloured wireframes, grouped by unit type. As in Brood War, a box that
  catches units leaves buildings out. Click a wireframe in the grid to select just that unit,
  `shift`-click to drop it, `ctrl`-click to keep only units of its type.
- **Graphs** (`g`): a large panel with tabs for income (minerals and gas per minute,
  smoothed over about 7-8 s), unit value, army value, workers and value lost over game
  time; it opens on unit value. For games between [Pluto](https://github.com/tscmoo/pluto)
  bots, a sixth tab plots each bot's own win estimate (its network's value output, shown as
  0-100%), read from the stats files it writes, `pluto_bandit_<opponent>.txt`. They are
  looked for beside the replay, in its `bot-write/` or `write/`, and in the self-play layout
  `<game>/{host,join}/bot-write/` (also through a symbolic link to the replay). Each game logs 64 points, so the line is coarse (about one
  point every 15-25 s).
  `d` switches to the first derivative (change per minute). `f` plots the first player
  minus the second, coloured by whoever is ahead. `m` pins the graphs minimized in the
  top-left corner of the view, where they keep updating (gas income dotted, as there is no
  legend); `g` or a click then maximizes / minimizes them instead of closing them, and `m`
  on the full panel unpins. Graph keys act only on the full panel.
  `r` switches units, army and losses between resource value (minerals + gas, with
  Zergling/Scourge pairs, morphs and Archons valued correctly; Overlords count as supply,
  like depots and pylons, not as units) and supply.
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
- **Remastered graphics** (`F2`): with a StarCraft: Remastered install on the machine, the
  map, units, buildings and effects are drawn from its HD art instead of the 1998
  sprites, at the window's resolution (the half-resolution HD2 set up to 2x zoom, full HD
  above). See *Remastered graphics* below.
- **Settings menu** (`?`): checkboxes for every toggle (overlay, selection panel, health
  bars, fog, two-colour mode, auto observer, mute, graph modes) and a scroll speed slider
  (keyboard and edge scrolling; default twice the original speed). Choices, whether made
  there or with the keys, are saved to `$XDG_CONFIG_HOME/openbw-viewer/settings`
  (`~/.config/...`; `%APPDATA%\openbw-viewer\settings` on Windows) and restored next time.
- **Minimap** flush in the bottom-left corner; click or drag on it to move the camera. It is
  drawn at the window's resolution, each tile its terrain art averaged over a 4x4 grid (the
  remastered tiles with those graphics on). Buildings are flat tiles in the
  units' colour, part transparent, inside a dark border; units are dots on top, air army outlined in black.
  Players' units and buildings are drawn 1.2x their footprint. Spider
  Mines are tiny solid dots; Interceptors, Scarabs, larvae and eggs are faint dots, and workers mining within
  12 tiles of one of their own bases, and SCVs constructing, are drawn as small tiles like the buildings,
  so armies stand out. A concentration of one
  unit type gets a round badge with that type's icon (a pill for several types): each type's units are grouped (linked within 6 tiles,
  DBSCAN-style, and cut by 2-means until no group is wider than 10 tiles), and a group shows
  when its cost reaches 800 minerals + gas and its density 12 per square tile. Icons of one
  player at about the same spot share a row (20 Zerglings under 8 Mutalisks: both, the
  costlier first); where rows would overlap, only the costlier is drawn. The grouping is in
  `ui/minimap_clumps.h`.
- **UI theme** (settings menu: auto, Terran, Protoss, Zerg; saved as `ui_theme=1..4`): with
  Remastered graphics, the panels, minimap frame, buttons and command icons wear the race's
  colours, and the progress bars use that race's own Remastered bars. Remastered has no
  race variants of the observer panels, so those are the one observer art recoloured. Auto
  follows the first player's race. The music follows the theme too (the playlist restarts
  when the theme's race changes). The classic graphics are unchanged.
- **Order lines** (`l`: off, selected units, all units): a dotted line from each unit to its
  order's target and on through queued orders, coloured by kind (move green, attack red,
  spell violet, build and repair cyan, gathering amber for selected units only). Builders
  outline the footprint they are placing; buildings show their rally line and an icon with a
  progress bar for what they are producing. With selected units only, a line whose target is
  off the screen is also drawn on the minimap.
- **Brood War's cursor**: the animated green arrow, from Remastered with its graphics on.
- **Replay bar** in the bottom-right corner: slower / pause-play / faster buttons, game timer,
  speed or pause state, and a seek bar. Hover the seek bar to see the game time under the cursor.
  The seek bar marks where the first unit died (cyan line) and each big battle (red strips
  above and below the bar, more solid the more was lost; at least 2000 minerals + gas of units within 20 s, up to 12
  per game, at least 30 s apart). Hover a marker to see whose unit it was, or each player's
  losses in the battle; click it to jump to 5 s before. They are found by playing the whole
  replay once in the background at startup (a few seconds), so they appear shortly after
  the window opens. With the Pluto bots' win estimates (see the Win % graph), the bar also
  marks when the game was decided: a gold tick where the eventual winner's chance (the mean
  of both bots' estimates) rose to 90% and stayed there to the end, with the rest of the bar
  shaded. Hover it for the time left; click it to skip to the end. Games decided only in
  their last minute (the loser resigning) get none.
- **Game menu** (`F10`): pauses the replay with **Return to Game**, **Options** (the
  settings menu) and **Exit Replay**, stacked as in the game's own menu; `F10` or `Esc`
  returns, playing or paused as before. With Remastered graphics the card uses the race
  theme's panel and the race's own dialog button art (`HD2/game/consoles/<race>/dlg.dds.grp`);
  the end-of-replay card shares it. Protoss and Zerg panels are cut from that race's console
  art (`console_left/center.dds`) rather than recoloured Terran metal (`ui/skin_strip.h`).
  Layout maths is in `ui/modal_layout.h`; tests: `viewer/tests/modal_layout_test.cpp`,
  `skin_strip_test.cpp`.
- **Music**: the race soundtracks of the players in the replay, shuffled, from Remastered's
  `music/*.ogg` when an install is found (the classic data files have none). The master
  volume and mute apply; **Music** in the settings menu turns it off on its own.
- **End of replay**: when playback reaches the end, a card gives each player's result
  (victorious, defeated, dropped; a player left standing after the others left is the
  winner) with **Watch Again** and **Exit Replay**. `Esc` closes it; it shows again at the
  end.
- **Observer console skin**: with the remastered graphics on, the stats overlay, selection
  panel, graphs, replay bar and minimap frame are drawn on Remastered's own observer-mode UI
  art (metal edge, corner rivets, red inner line), with its green progress bars and its HD
  command icons, tinted gold as in the game. With the classic graphics they keep a bevelled
  olive style.
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

## Remastered graphics

The viewer reads the art straight from an installed StarCraft: Remastered (the free
Battle.net download; under Wine on Linux). It looks for the install in
`$OPENBW_REMASTERED_DIR` (the folder holding `.build.info`), then in
`C:/Program Files (x86)/StarCraft` (Windows), `/Applications/StarCraft` (macOS), or
`~/.wine/drive_c/Program Files (x86)/StarCraft` and Lutris's
`~/Games/battlenet/drive_c/Program Files (x86)/StarCraft` (Linux). Without one, or with
`F2` off, the classic graphics are used. The game still runs from the 1.16.1 MPQs; the
install only supplies graphics.

What is drawn from Remastered: terrain (HD megatiles and creep edges), every images.dat
sprite with team colour, shadows, cloaking, warp-in and warp flash, fire, smoke and
explosions, selection circles, selection-panel wireframes and the mouse cursor. Sprites are composited the way Remastered's own
shaders do it (team colour mask, shadows that darken by up to half without stacking).
Not reproduced: the dynamic lighting from normal/specular maps, animated water, and HD
health bars (the classic ones are drawn over the HD view). Assets load the first time
they are seen and stay block-compressed; frames are decoded and scaled as they are drawn,
with both kept within fixed budgets (384 MB of atlases, 256 MB of scaled frames; past
one, what was drawn least recently is dropped). Over a whole
game this measured about 110 MB more than the classic graphics at 1.5x zoom and 280 MB more
at 3x.

## Build

Requires CMake, a C++14 compiler and SDL2. SDL2_mixer is optional and enables sound;
zlib is optional and enables the remastered graphics.

```sh
# Arch: pacman -S cmake sdl2 sdl2_mixer zlib    Debian/Ubuntu: apt install cmake libsdl2-dev libsdl2-mixer-dev zlib1g-dev
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
| `F10` | game menu (pauses; Options opens the settings menu) |
| `=` / `-` (also `a` / `z`) | faster / slower: ×2 per press from 1/4× to 128×, with 1.5× between 1× and 2× |
| `ctrl =` / `ctrl -` / `ctrl 0` | zoom in / out / reset to 1× (starts at 1.5×; the last zoom is remembered) |
| mouse wheel | zoom around the cursor |
| arrows, or the mouse at the game view's edge | scroll (with the full graph panel open, `←` / `→` switch its tabs) |
| `backspace` | rewind about 5 s |
| `page down` / `page up` | skip forward / back 10 s |
| click or drag the replay bar | seek |
| click / drag / `ctrl`-click / `shift`-click | select (`ctrl`-click selects the same type on screen) |
| click a wireframe in the selection grid | select that unit (`shift` drop it, `ctrl` keep its type) |
| click the wireframe of a single selected unit | centre the view on it |
| `F2` | remastered or classic graphics (remastered when an install is found) |
| `b` | health bars under all units, or only selected ones (all by default) |
| hold `Alt` | health bars under all units while held, when `b` has them on selected units only (as in Warcraft III) |
| `i` | toggle the selection panel |
| `g` / `esc` | toggle / close the graphs (`g` on the minimized panel restores it); `tab`, `←` / `→` or `1`-`5` (`6` with Pluto data; or click a tab) to switch |
| `r` | graphs: resource value or supply |
| `d` | graphs: level or rate of change (with the graphs closed, `d` slows playback) |
| `f` | graphs: players' difference or a line each (with the graphs closed, fog of war) |
| `o` | auto observer on / off |
| `l` | order lines: off, selected units, all units |
| `[` / `]`, or the wheel over the graphs | zoom the graphs' time axis out / in |
| `shift tab` | two-colour mode: the two players in red and teal (again to restore) |
| `m` | full graph panel: pin minimized in the corner / unpin (otherwise mute) |

Playback starts at 1×, so press `=` a few times.

## Notes

- Kill counts: upstream OpenBW never increments them; this fork credits the killing
  unit (an Interceptor's or Scarab's kills go to its Carrier or Reaver). Spells count for
  their caster (Psionic Storm, Irradiate, Feedback, Spawn Broodling), an Archon starts with
  its two Templar's kills, and spellcasters show their count too. Kill counts are
  display only, so replays still play back identically.

- The overlay font in `ui/hud_font.h` is a bitmap rendering of JetBrains Mono Bold
  (SIL OFL 1.1, see `LICENSE-JetBrainsMono.txt`). Regenerate it with `gen_hud_font.py`.
- `ui/order_names.h` is generated from `bwenums.h` by `gen_order_names.py`.
- Wireframes come from the game's own `unit/wirefram/*.grp` files (with the remastered
  graphics, from Remastered's `wirefram.dds.grp`, coloured the same way). Colours are remapped
  through `game/twire.pcx` by missing health and shields, as the game does. This covers
  the blue shield outline, green/yellow/red hull sections, and the Zerg heat-map
  ramp, using the tables documented on
  [staredit.net](https://staredit.net/topic/18436/). Which hull section changes first
  is an approximation: the per-unit pattern is undocumented. Remastered's Shuttle has no
  line art in its wireframe, only filled hull sections, which are drawn as the line art.

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
