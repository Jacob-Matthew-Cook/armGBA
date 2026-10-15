# armGBA

A Game Boy Advance libretro core built around a recompiler, written for small ARM handhelds. Game Boy
and Game Boy Color games run in the same core.

The aim is timing a game cannot tell from the hardware, at twice the speed of mGBA on a Cortex-A35 or
Cortex-A55.

## How it works

**Recompiler with an exact fallback.** ARM and Thumb code is compiled to AArch64 or x86-64 and counts
its own cycles. When an event (timer, video line, DMA, IRQ) falls inside a block, a copy of the block
with a check before each instruction takes over. An event the instruction cannot observe runs after
it; one it can observe has that single instruction run by a cycle-exact stepping CPU (`src/step.c`).

**Fewer events.** On a small core an event costs far more than its own work, because handling it
pushes the game out of the instruction cache. Sound-timer overflows that only feed a FIFO or tick a
cascaded timer are batched. Video events are scheduled only where something can see them, and lines
are drawn lazily. `DISPSTAT`, `VCOUNT`, `KEYINPUT` and `IF` are read inline while they cannot change.
Provably idle loops skip to the next event, and so do waiting loops that span calls once the exact CPU
has seen one go round twice the same way.

**Self-modifying code.** Writes into compiled code are caught by a per-word map. An instruction whose
operand keeps changing is compiled to read it from guest memory; a block that alternates between a
few shapes keeps a compiled form of each; anything else is recompiled.

**Small caches.** Block bodies are packed together and slow paths kept elsewhere; sequences every
block would repeat are shared routines; guest memory is page-aligned.

**The bus.** Every access costs what the hardware's would: wait states, sequential or not, the
cartridge prefetch buffer, and the wait for H-blank when the display has VRAM to itself.

**Video and audio.** A line renderer with NEON and AVX2 paths that redraws only lines whose inputs
changed, optionally on a second thread. Audio is mixed in batches between changes.

**Saves.** The save type is detected on first use, with mGBA's override list. Save files are
interchangeable with mGBA's core. An EEPROM reports busy for about 7 ms after a write, and Flash for
1.8 ms after a sector erase, as mGBA has them. A Game Boy cartridge's clock (MBC3 or HuC3) is kept in
the `.rtc` file and runs on by the time the game was off; an MBC3 file reads gambatte-libretro's
`.rtc`, and that core reads armGBA's. A host clock that was never set, or that went back, adds no
time. While a game runs, its clock counts emulated time, so fast-forward speeds it up.

**Cartridge hardware.** The Matrix mapper of the 64 MiB Game Boy Advance Video cartridges, the bank
register of multi-game cartridges, Vast Fame's protection, and the mirrored ROM of the Classic NES
Series. The sensors and motor some cartridges carry, as mGBA has them: Boktai's light sensor,
WarioWare: Twisted!'s gyro, the tilt sensor of Yoshi Topsy-Turvy and Korokoro Puzzle, and the rumble
of Drill Dozer and WarioWare: Twisted!. A stick (or the D-pad) tilts and turns the cartridge, as
mGBA's SDL frontend scales a stick; the right stick, pushed up or down, changes the sunlight while playing.

**Game Boy and Game Boy Color.** Gambatte-Speedrun's core (`gambatte/`), plus mappers for unlicensed
cartridges (Sachen, TAMA5, MBC6, MBC7, Vast Fame, and the BBD, Hitek, Sintax, Li Cheng, NT and Makon
families), a choice of console model and screen, and a tilt sensor mapping. The screens other than
Grayscale (gambatte-libretro's) and the Game Boy Color's (and Game Boy Advance's) color correction are
SameBoy's, and so are the free boot ROMs built in.

## Accuracy

Anything a game's logic or the picture can observe is exact: IRQ, DMA and timer timing, self-modifying
code, the prefetch pipeline, visible video timing. What neither can observe is simplified where that
is free: audio is exact in content but batched in time.

Where the hardware gives no answer, these rules apply:

- **Game Boy model (Auto):** Game Boy Color if the header's color flag is `C0`, or `80` unless that
  byte is really the end of an older game's title. Otherwise Game Boy.
- **Unlicensed Game Boy mappers:** chosen by the header logo's checksum, bank-end markers and, for a
  few dumps, the header's CRC.
- **EEPROM size:** the small one until an address past its end is used.

## Options

A game sees only its own console's options, in this order. Holding **L2** or **R2** fast-forwards, and **X** and **Y** are turbo **A** and **B**. In Game Boy Advance games **L3** and **R3** are turbo **L** and **R**.
A Game Boy game running as on a Game Boy Advance is shown at its own size (with the black around it
if Game Boy Advance Border is Shown), and **L** or **R** stretches it across, as the console's
shoulder buttons do.

| Console | Option | Values (default first) |
| --- | --- | --- |
| Game Boy Advance | Turbo Button Period | 4 to 120 frames, 4 by default |
| | Skip BIOS Intro | Disabled, Enabled |
| | Threaded Video | Disabled, Enabled, One Frame Late |
| | Color Correction | Disabled, Enabled |
| | Tilt Sensor | Left Stick, Right Stick, D-Pad |
| | Solar Sensor | 0 (Dark) to 10 (Full Sun), 5 by default |
| Game Boy, Game Boy Color | Turbo Button Period | 4 to 120 frames, 4 by default |
| | Game Boy Model (applies on restart) | Auto, Game Boy, Game Boy Pocket / Light, Game Boy Color, Game Boy Advance |
| | Boot ROM (applies on restart) | On (Nintendo's, from the system directory), Off, Open-Source (SameBoy's, built in) |
| | Color Correction | Disabled, Enabled |
| | Game Boy Screen | Grayscale, Game Boy (Green), Game Boy Pocket (Olive), Game Boy Light (Teal) |
| | Game Boy Advance Border | Cropped, Shown |
| | Screen Off Behavior | As Hardware, Background Color |
| | Tilt Sensor | Left Stick, Right Stick, D-Pad |

## Cheats and achievements

The frontend's cheat codes work on both sides: GameShark, Action Replay and CodeBreaker codes,
encrypted or not, for the Game Boy Advance, decoded as mGBA decodes them and applied as each frame
ends; Game Genie and GameShark codes for the Game Boy. The memory is described to the frontend as
mGBA's and gambatte-libretro's cores describe it, so achievements read the same addresses.

## Performance

Frames per second on one core, unthrottled, against the cores ROCKNIX ships. Game Boy Advance
games ran 9000 frames with scripted button presses; Game Boy games 3000 frames from power-on.

Anbernic RG353M (RK3566, Cortex-A55 1.8 GHz), 14 October 2026:

| Game | armGBA | mGBA | |
| --- | --- | --- | --- |
| Payback | 232 | 148 | 1.57x |
| Mario Kart: Super Circuit | 372 | 248 | 1.50x |
| Final Fantasy Tactics Advance | 353 | 255 | 1.38x |
| Advance Wars | 320 | 202 | 1.58x |
| Golden Sun | 549 | 347 | 1.58x |

| Game | armGBA | Gambatte | mGBA |
| --- | --- | --- | --- |
| Tetris | 1918 | 1669 | 857 |
| Pokémon Red | 2555 | 1877 | 1206 |
| Super Mario Land 2 | 2260 | 1869 | 1021 |
| The Legend of Zelda: Link's Awakening | 1932 | 1593 | 903 |
| Pokémon Crystal | 1737 | 1481 | 911 |
| The Legend of Zelda: Link's Awakening DX | 1440 | 1375 | 679 |
| Shantae | 869 | 901 | 481 |
| Wario Land 3 | 1532 | 1449 | 734 |

With Threaded Video, Payback on the RG353M runs at 276, and at 288 with One Frame Late. Every game
here runs several times faster than full speed. The 2x target over mGBA is met on x86-64, and not
yet on one RK3566 core, where the small caches decide most of the speed.

## Testing

- **Lockstep:** twenty games compared frame by frame with the stepping CPU, on both backends, and
  every game of the Game Boy Advance set (4590) for 1500 frames on x86-64.
- **`tests/cpufuzz`:** random ARM and Thumb programs against mGBA.
- **Hardware test ROMs:** all 28 pass (jsmolka's gba-tests, FuzzARM, NanoBoyAdvance's).
- **mGBA test suite:** every scored category passes in full except Misc edge, at 6 of 12.
- **Game Boy:** 213 of 236 test ROM screens match a SameBoy-based build; the rest are SameSuite's
  APU tests, which time the sound channels to a fraction of a CPU cycle. Every game of the Game Boy
  and Game Boy Color sets was started and compared with SameBoy: 2776 of 2796 and 2983 of 3055 match.
  The rest are not games (BIOS files, cheat cartridges), betas, and unlicensed carts that differ on
  the other cores too.
- **Save states and runahead:** frame-exact on both backends for GBA, GBC and GB. A Game Boy state
  made as one model loads as another: the machine becomes the state's model until the next restart.

## Building

    cmake -S . -B build -G Ninja
    ninja -C build

This makes `armgba_libretro.so`; `libretro/armgba_libretro.info` goes in RetroArch's info directory.
The system directory needs `gba_bios.bin`. Game Boy boot ROMs there are optional: `gb_bios.bin` or
`dmg_boot.bin`, `gbc_bios.bin` or `cgb_boot.bin`, `mgb_boot.bin`, and for the Game Boy Advance model
`cgb_agb_boot.bin`, `cgb_agb0_boot.bin`, the Game Boy Color's (made into it) or `agb_boot.bin`, in
that order. With Boot ROM On, Nintendo's is used under whichever of these names it has, a SameBoy
boot ROM under one of them only where Nintendo's isn't there, and with neither the game starts
without one.

The test tools also need an mGBA source tree with `tests/mgba-oracle.patch` applied, and its
`libmgba.a` built with `-DCOLOR_16_BIT`:

    cmake -S . -B build -G Ninja -DMGBA_DIR=<mgba> -DMGBA_LIB=<mgba-build>/libmgba.a -DMGBA_BUILD_INCLUDE=<mgba-build>/include

## Diagnostics

Environment variables for working on the core, read at start:

| Variable | Effect |
| --- | --- |
| `ARMGBA_STATS=1` | Recompiler counts at exit: blocks, exits, steps, code writes |
| `ARMGBA_RENDER=reference` | Draw with mGBA's loops only; `=check` draws both ways and stops at the first difference |
| `ARMGBA_PERFMAP=1` | Name compiled blocks in `/tmp/perf-<pid>.map` for `perf` |
| `ARMGBA_DUMP=<dir>` | Write each compiled block's host code there, one file a block |
| `ARMGBA_SMCLOG=1` | Log writes into compiled code |
| `ARMGBA_STEP=<mask>` | Leave the instruction kinds in the hex mask to the exact CPU |
| `ARMGBA_TRACE=<file>` (test tools) | Log each instruction and its time; `TRACE_FROM`, `TRACE_TIME` and `TRACE_COUNT` say where it starts and how long it runs |

## Known gaps

- A few unlicensed Game Boy Color cartridges with undocumented protection chips do not boot.
- Cheat codes mGBA can't run either (Action Replay's slowdown and disabling codes) are left out, and
  a code that hooks a routine of the game runs as the frame ends instead.

## Credits

Code taken from other projects, each under its own license (see License):

- **mGBA** (Jeffrey Pfau and contributors, MPL-2.0): the software renderer, bus and I/O behaviour,
  save chips, the GPIO clock, the light sensor, gyro, tilt sensor and rumble, the Matrix and Vast
  Fame cartridge mappers, the save override list,
  the GBA sound channels, the cheat code decoders and engine, and the unlicensed Game Boy mapper
  tables. It is also the yardstick
  the GBA side is compared with, through `tests/mgba-oracle.patch`.
- **Gambatte** (sinamas) as extended by **Gambatte-Speedrun** (pokemon-speedrunning/gambatte-core,
  GPL-2.0): the whole Game Boy core in `gambatte/`, with **snes_spc** (Shay Green, LGPL) inside it.
- **SameBoy** (Lior Halphon, MIT): the Game Boy screen palettes other than Grayscale, the Game Boy Color and Game Boy
  Advance color correction curves, and the free boot ROMs in `libretro/sameboy-boot.h`. A SameBoy-based build is also the reference the Game Boy side is
  tested against.
- **libretro** (MIT): `libretro/libretro.h`.

Read for how the hardware behaves, without code taken:

- **NanoBoyAdvance** (fleroviux): its hardware test ROMs, which the sprite mosaic rule was worked
  out from, and its renderer, read for when a layer switched on mid-line shows.
- **hhugboy** (taizou): how unlicensed Game Boy cartridges are detected and banked.
- **gambatte-libretro**: its `.rtc` layout, which armGBA's clock files share, its turbo buttons and
  their period, and its Grayscale palette.
- **pret/pokecrystal** and **pret/pokegold**: how Pokémon Gold, Silver and Crystal check the clock.
- **gpSP**: fast-forward on a held button.

Test ROMs: jsmolka's gba-tests, DenSinH's FuzzARM, NanoBoyAdvance's hardware tests, mGBA's test suite and its
Game Boy test collection (Blargg's tests, Gekkio's Mooneye, LIJI's SameSuite).

## AI assistance

armGBA was written with heavy use of an AI coding assistant, Anthropic's Claude (through Claude
Code). Most of the code, comments and documentation were drafted by it at Jacob Cook's direction.
He chose what to build and decides what goes in, and each change was tested on his PCs and
handhelds first; the accuracy and speed claims here come from those tests, listed above, not from
the assistant's word.

## License

Distributed under the GNU General Public License, version 2 (`LICENSE`), because the core links
Gambatte. Each part keeps its own notice:

| Part | License |
| --- | --- |
| `src/`, `tests/`, the `libretro/` glue | Mozilla Public License 2.0 (`LICENSES/MPL-2.0.txt`); files that follow mGBA's code carry its copyright notice too |
| `gambatte/` | GPL, version 2 (`gambatte/COPYING`) |
| `gambatte/libgambatte/src/mem/snes_spc/` | GNU LGPL |
| `libretro/libretro.h` | MIT |
| The Game Boy palettes (other than Grayscale) and color curves in `libretro/gb-gambatte.cpp`, the boot ROMs in `libretro/sameboy-boot.h` | MIT, from SameBoy (Lior Halphon) |
