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
  smoothed over game time and refreshed every 4 game frames, scores recent damage, army
  units that have already picked an enemy as their target (a second or two ahead of the
  first damage), armies that are fighting or about to meet, and army size; in quiet spells,
  scouting workers and new buildings, so the camera tours the map rather than sitting
  still. A clearly bigger scene (1.3x) takes over within about half a second, a much bigger
  one sooner; a quiet shot gives way to a fight at once, while one quiet place waits 4 s
  before the next so a tour does not flicker. The camera moves every frame: two critically
  damped springs in series, so a pan eases in and out without a snap, with a soft dead zone
  (about 1-8% of a view) so wobbles of the scene's centre do not shake the picture. Pans are
  quick, so a fight a view or two away is reached in well under a second; only scenes more
  than 3.5 views away are cut to, as a human observer clicks the minimap. Timing speeds up
  with the playback speed. Moving the camera yourself pauses it for a few seconds.
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
  remastered tiles with those graphics on). Buildings are flat tiles in the units' colour,
  part transparent, inside a dark border; units are dots on top. Flyers cast a soft shadow
  below and to the right, and air army is outlined in black. Minerals and geysers are drawn
  over buildings and ground units, so a mineral line stays solid under its workers; flyers
  stay on top of them. Players' buildings
  are drawn 1.2x their footprint and units 1.6x (1.3x that for units costing 250+ minerals
  and gas, 0.75x under 100); overlapping buildings of one player, and its workers drawn in
  their style, merge into one shape with a single outer border. Spider Mines are tiny solid
  dots; Interceptors, Scarabs, larvae and eggs are faint dots; workers within 10 tiles of one
  of their own buildings (whatever they are doing), and Overlords, are drawn as small tiles
  like the buildings, so armies stand out. Very close dots of one player's units of one type
  (overlapping by about 70%; three or more) combine into one larger dot whose area follows
  their total minerals + gas on one scale for every type (a mid-sized unit's dot stands for
  150; never smaller than one member, at most 4x that dot) (`ui/minimap_merge.h`, tested by `viewer/tests/minimap_merge_test.cpp`).
- **UI theme** (settings menu: auto, Terran, Protoss, Zerg, Minimal; saved as `ui_theme=1..5`):
  **Minimal**, the default, uses no game art: flat dark panels with a hairline edge, neutral
  greys and one blue accent (the seek bar, progress bars, what is on in the settings menu).
  It is also the look without a Remastered install. The race themes, with Remastered
  graphics, dress the panels, minimap frame and buttons in the race's colours (command icons
  stay yellow under every theme) and use the race's own Remastered progress bars; Remastered
  has no race variants of the observer panels, so those are the one observer art recoloured.
  Auto follows the first player's race. The music follows the theme's race (Minimal's is the
  first player's; the playlist restarts when it changes). The game menu and end card look
  like the game's own under every theme. Colours: `ui/ui_theme.h`.
- **FPS counter** (settings menu, on by default; saved as `show_fps`): small, top right,
  under the stats panel: frames drawn per second (over half a second, `ui/fps_meter.h`) and
  the display refresh the viewer paces itself to, one frame per refresh ("60 Hz?" when the
  display does not report one and 60 is assumed). The small font is `ui/hud_font_small.h`
  (JetBrains Mono Bold 12 px, from `viewer/gen_hud_font.py`).
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
- **Game menu** (`F10`): pauses the replay with **Options** (`O`, the settings menu,
  returning here when it closes), **Exit Replay** (`E`) and **Return to Game** (`Esc` or
  `F10`), which resumes playing or paused as before; other keys do nothing while it is open.
- **End of replay**: when playback reaches the end, a card gives each player's result
  (victorious, defeated, dropped; a player left standing after the others left is the
  winner) with **Watch Again** (`W`) and **Exit Replay** (`X` as in the game, or `E`). `Esc`
  closes it; it shows again at the end.
- Both cards are drawn like the game's own Game Menu (its measurements, from
  `rez/gamemenu.ui.json`, scaled to the window): the map darkened under a navy veil, buttons
  outlined twice in blue, yellow labels with the hotkey letter in white, the default button
  set apart with a brighter ring. The text is the game's Eurostile, read at run time from a
  Remastered install (`font/eurostile-reg.ttf`, rendered with the vendored public-domain
  `ui/third_party/stb_truetype.h` through `ui/tt_font.h`); without an install, the viewer's
  own font. Layout and hotkeys: `ui/modal_layout.h`; tests: `viewer/tests/modal_layout_test.cpp`,
  `tt_font_test.cpp`.
- **Race frames**: with Remastered graphics, Protoss and Zerg panels (and the minimap's top
  and right edges) are framed like the race's main screen, cut from its console art
  (`console_center.dds`): the blue bezel and cyan rail under the gold rail and leaves, or the
  flesh ring, mirrored into a nine-slice frame (`ui/skin_frame.h`, tested by
  `viewer/tests/skin_frame_test.cpp`); panels pad their content to clear it.
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

- The overlay font in `ui/hud_font.h` is an 18px bitmap rendering of JetBrains Mono Bold
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

The logic is in `ui/observer_camera.h` (camera springs and the shot director, tested by
`tests/observer_camera_test.cpp`); the interest map is in `ui/ui.h`. `metrics.py` also
reports, per fight of at least 600 hp, `lag`: game seconds from its first damage until it is
framed (negative: the camera was already there; clipped at -3, and 10 if never framed within
10 s), `on<=1s` the share of fights framed within a second, `still`/`maxstill` how much of
the time the camera does not move, and `rms_acc` its acceleration. Typical results on five
Pluto self-play replays (old to new): mean lag 4.8 to 2.4 s at 1x and 5.5 to 3.4 s at 8x;
fights framed within 1 s 36% to 54% at 1x; camera still 87% to 55% of the time at 1x.
