# armGBA

A Game Boy Advance, Game Boy and Game Boy Color libretro core for low-power handhelds.

armGBA builds on years of work by others. Parts of its Game Boy Advance side come from mGBA (Jeffrey
Pfau and contributors), notably the renderer, cheat engine, sound channels and save chips, and mGBA
is the reference it's tested against. Its Game Boy and Game Boy Color games run on Gambatte-Speedrun's
core. See Credits for the full list.

What armGBA adds is a recompiler for the Game Boy Advance's CPU, so games run with headroom on slow
handheld chips such as the Cortex-A35 and Cortex-A55. It stays accurate wherever a game could notice:
interrupts, DMA, timers and what's on screen all happen when they would on the hardware.

## Controls

| Button | Game Boy Advance | Game Boy, Game Boy Color |
| --- | --- | --- |
| L2, R2 (held) | Fast-forward, up to RetroArch's Maximum Run Speed | Same |
| X, Y (held) | Turbo A, turbo B | Same |
| L3, R3 (held) | Turbo L, turbo R | Click of the tilt stick: flick (motion sensor) |
| L, R | L, R | Stretch the picture (Game Boy Advance model) |
| Tilt stick | Tilt and turn (motion sensor games) | Tilt (motion sensor games) |
| Right stick up, down | Sunlight up, down (solar sensor games) | — |

The tilt stick is the left stick by default. The Tilt Sensor option moves it to the right stick or
the D-pad; with the D-pad, L is the flick.

## Options

A game sees only its own console's options, in this order.

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

On the Game Boy Advance model, a Game Boy game is shown at its own size. With Game Boy Advance Border
set to Shown, the black around it shows too, as on that console's screen.

## Building

    cmake -S . -B build -G Ninja
    ninja -C build

This makes `armgba_libretro.so`. `libretro/armgba_libretro.info` goes in RetroArch's info directory.

The system directory needs `gba_bios.bin`. Game Boy boot ROMs are optional, looked for under these
names in this order:

- Game Boy: `gb_bios.bin`, `dmg_boot.bin`
- Game Boy Pocket: `mgb_boot.bin`
- Game Boy Color: `gbc_bios.bin`, `cgb_boot.bin`
- Game Boy Advance: `cgb_agb_boot.bin`, `cgb_agb0_boot.bin`, then the Game Boy Color's (converted),
  then `agb_boot.bin`

With Boot ROM On, Nintendo's boot ROM is used under whichever of these names it has. A SameBoy boot
ROM under one of them is used only where Nintendo's isn't there. With neither, the game starts
without one.

## Features

**Cheats and achievements.** The frontend's cheat codes work on both consoles. For the Game Boy
Advance: GameShark, Action Replay and CodeBreaker codes, encrypted or not, decoded as mGBA decodes
them and applied as each frame ends. For the Game Boy: Game Genie and GameShark codes. The memory is
described to the frontend as mGBA's and gambatte-libretro's cores describe it, so achievements read
the same addresses.

**Saves.** The save type is detected on first use, with mGBA's override list. Save files are
interchangeable with mGBA's core. An EEPROM reports busy for about 7 ms after a write, and Flash for
1.8 ms after a sector erase, as in mGBA.

**Clocks.** A Game Boy cartridge's clock (MBC3 or HuC3) is kept in the `.rtc` file and moves on by the
time the game was off. armGBA and gambatte-libretro read each other's MBC3 `.rtc` files. A host clock
that was never set, or that went back, adds no time. While a game runs, its clock counts emulated
time, so fast-forward speeds it up.

**Game Boy Advance cartridge hardware.** The Matrix mapper of the 64 MiB Game Boy Advance Video
cartridges, the bank register of multi-game cartridges, Vast Fame's protection, and the mirrored ROM
of the Classic NES Series. Also the sensors and motor some cartridges carry, as mGBA has them:
Boktai's light sensor, WarioWare: Twisted!'s gyro, the tilt sensor of Yoshi Topsy-Turvy and
Korokoro Puzzle, and the rumble of Drill Dozer and WarioWare: Twisted!. A stick moves them the way
mGBA's SDL frontend scales a stick.

**Game Boy and Game Boy Color.** Gambatte-Speedrun's core (`gambatte/`), plus mappers for unlicensed
cartridges: Sachen, TAMA5, MBC6, MBC7, Vast Fame, and the BBD, Hitek, Sintax, Li Cheng, NT and Makon
families. The screen palettes, apart from Grayscale (gambatte-libretro's), are SameBoy's. So are the
Game Boy Color and Game Boy Advance color correction and the free boot ROMs built in.

## Accuracy

Anything a game's logic or the picture can observe is exact: IRQ, DMA and timer timing, self-modifying
code, the prefetch pipeline, and visible video timing. What neither can observe is simplified where
that is free: audio is exact in content but batched in time.

Where the hardware gives no answer, these rules apply:

- **Game Boy model (Auto):** Game Boy Color if the header's color flag is `C0`, or `80` unless that
  byte is really the end of an older game's title. Otherwise Game Boy.
- **Unlicensed Game Boy mappers:** chosen by the header logo's checksum, bank-end markers and, for a
  few dumps, the header's CRC.
- **EEPROM size:** the small one until an address past its end is used.

## Testing

- **Lockstep:** twenty games compared frame by frame with the stepping CPU on both backends, and
  every game of the Game Boy Advance set (4590) for 1500 frames on x86-64.
- **`tests/cpufuzz`:** random ARM and Thumb programs against mGBA.
- **Hardware test ROMs:** all 28 pass (jsmolka's gba-tests, FuzzARM, NanoBoyAdvance's).
- **mGBA test suite:** every scored category passes in full except Misc edge, at 6 of 12.
- **Game Boy test ROMs:** 213 of 236 screens match a SameBoy-based build. The rest are SameSuite's
  APU tests, which time the sound channels to a fraction of a CPU cycle.
- **Game Boy games:** every game of the Game Boy and Game Boy Color sets was started and compared with
  SameBoy: 2776 of 2796 and 2983 of 3055 match. The rest are not games (BIOS files, cheat cartridges),
  betas, and unlicensed carts that differ on the other cores too.
- **Save states and runahead:** frame-exact on both backends for GBA, GBC and GB. A Game Boy state
  made as one model loads as another: the machine becomes the state's model until the next restart.

## Performance

How much headroom armGBA leaves on a slow handheld: frames per second on one core, unthrottled, with
the cores ROCKNIX ships alongside for reference. Full speed is about 60. Game Boy Advance games ran
9000 frames with scripted button presses, Game Boy games 3000 frames from power-on.

Anbernic RG353M (RK3566, Cortex-A55 1.8 GHz), 14 October 2026:

| Game | armGBA | mGBA |
| --- | --- | --- |
| Payback | 232 | 148 |
| Mario Kart: Super Circuit | 372 | 248 |
| Final Fantasy Tactics Advance | 353 | 255 |
| Advance Wars | 320 | 202 |
| Golden Sun | 549 | 347 |

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
here runs several times faster than full speed, which leaves room for runahead, fast-forward and
slower chips.

## How it works

**Recompiler with an exact fallback.** ARM and Thumb code is compiled to AArch64 or x86-64 and counts
its own cycles. When an event (timer, video line, DMA, IRQ) falls inside a block, a copy of the block
with a check before each instruction takes over. An event the instruction cannot observe runs after
it. For one it can observe, a cycle-exact stepping CPU (`src/step.c`) runs that single instruction.

**Fewer events.** On a small core an event costs far more than its own work, because handling it
pushes the game out of the instruction cache. Sound-timer overflows that only feed a FIFO or tick a
cascaded timer are batched. Video events are scheduled only where something can see them, and lines
are drawn lazily. `DISPSTAT`, `VCOUNT`, `KEYINPUT` and `IF` are read inline while they cannot change.
Provably idle loops skip to the next event. So do waiting loops that span calls, once the exact CPU
has seen one go round twice the same way.

**Self-modifying code.** A per-word map catches writes into compiled code. An instruction whose
operand keeps changing is compiled to read it from guest memory. A block that alternates between a
few shapes keeps a compiled form of each. Anything else is recompiled.

**Small caches.** Block bodies are packed together, with slow paths kept elsewhere. Sequences every
block would repeat are shared routines. Guest memory is page-aligned.

**The bus.** Every access costs what the hardware's would: wait states, sequential or not, the
cartridge prefetch buffer, and the wait for H-blank when the display has VRAM to itself.

**Video and audio.** A line renderer with NEON and AVX2 paths redraws only lines whose inputs
changed, optionally on a second thread. Audio is mixed in batches between changes.

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

The test tools also need an mGBA source tree with `tests/mgba-oracle.patch` applied, and its
`libmgba.a` built with `-DCOLOR_16_BIT`:

    cmake -S . -B build -G Ninja -DMGBA_DIR=<mgba> -DMGBA_LIB=<mgba-build>/libmgba.a \
          -DMGBA_BUILD_INCLUDE=<mgba-build>/include

## Known gaps

- A few unlicensed Game Boy Color cartridges with undocumented protection chips do not boot.
- Cheat codes mGBA can't run either (Action Replay's slowdown and disabling codes) are left out. A
  code that hooks a routine of the game runs as the frame ends instead.

## Credits

Code taken from other projects, each under its own license (see License):

- **mGBA** (Jeffrey Pfau and contributors, MPL-2.0): the software renderer, bus and I/O behaviour,
  save chips, the GPIO clock, the light sensor, gyro, tilt sensor and rumble, the Matrix and Vast Fame
  cartridge mappers, the save override list, the GBA sound channels, the cheat code decoders and
  engine, and the unlicensed Game Boy mapper tables. It is also the yardstick the GBA side is
  compared with, through `tests/mgba-oracle.patch`.
- **Gambatte** (sinamas) as extended by **Gambatte-Speedrun** (pokemon-speedrunning/gambatte-core,
  GPL-2.0): the whole Game Boy core in `gambatte/`, with **snes_spc** (Shay Green, LGPL) inside it.
- **SameBoy** (Lior Halphon, MIT): the Game Boy screen palettes other than Grayscale, the Game Boy
  Color and Game Boy Advance color correction curves, and the free boot ROMs in
  `libretro/sameboy-boot.h`. A SameBoy-based build is also the reference the Game Boy side is tested
  against.
- **libretro** (MIT): `libretro/libretro.h`.

Read for how the hardware behaves, without code taken:

- **NanoBoyAdvance** (fleroviux): its hardware test ROMs, which the sprite mosaic rule was worked
  out from, and its renderer, read for when a layer switched on mid-line shows.
- **hhugboy** (taizou): how unlicensed Game Boy cartridges are detected and banked.
- **gambatte-libretro**: its `.rtc` layout, which armGBA's clock files share, its turbo buttons and
  their period, and its Grayscale palette.
- **pret/pokecrystal** and **pret/pokegold**: how Pokémon Gold, Silver and Crystal check the clock.
- **gpSP**: fast-forward on a held button.

Test ROMs: jsmolka's gba-tests, DenSinH's FuzzARM, NanoBoyAdvance's hardware tests, mGBA's test
suite and its Game Boy test collection (Blargg's tests, Gekkio's Mooneye, LIJI's SameSuite).

## AI assistance

armGBA was written with heavy use of an AI coding assistant, Anthropic's Claude (through Claude
Code). Most of the code, comments and documentation were drafted by it at Magnominus's direction.
He chose what to build and decides what goes in. Each change was tested on his PCs and handhelds
first, and the accuracy and speed claims here come from those tests, listed above, not from the
assistant's word.

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
