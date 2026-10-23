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
| L, R, Start (held 1 s) | Restart that player's console (the Restart Buttons option) | Same |

With the consoles of a link on one device, R2 changes how the screens are shown and L2 fast-forwards,
or changes them back as the L2 Button option says; in netplay, L2 changes them back. The tilt stick
is the left stick by default. The Tilt Sensor option moves it to the right stick or the D-pad; with
the D-pad, L is the flick.

## Options

The options are in six categories, Link's in four parts: Link (Link Mode), Link: Players, Link:
Connection, and Link: Screens and Controls; the standalone frontend shows Link's as one page with
headers. The menu shows only those that do something for the game loaded and the Link Mode chosen:
the Tilt and Solar Sensors only for cartridges with one, the link options only with Link Mode on.

| Category | Option | Console | Values (default first) |
| --- | --- | --- | --- |
| System | Skip BIOS Intro | Game Boy Advance | Disabled, Enabled |
| | BIOS (applies on restart) | Game Boy Advance | Nintendo's (System Folder), armGBA's (Built In) |
| | Game Boy Model (applies on restart) | Game Boy | Auto, Game Boy, Game Boy Pocket / Light, Game Boy Color, Game Boy Advance |
| | Boot ROM (applies on restart) | Game Boy | Nintendo's (System Folder), Off, armGBA's (Built In) |
| Video | Threaded Video | Game Boy Advance | Disabled, Enabled, One Frame Late |
| | Color Correction | Both | Disabled, Enabled |
| | Game Boy Screen | Game Boy | Grayscale, Game Boy (Green), Game Boy Pocket (Olive), Game Boy Light (Teal) |
| | Game Boy Advance Border | Game Boy | Cropped, Shown |
| | Game Boy Advance Panel | Game Boy | Game Boy Advance, Game Boy Color |
| | LCD Ghosting | Both | Off, Half, Full |
| | Interleaved Rows | Both | Off, On |
| | LCD Grid | Both | Off, 2x to 8x |
| | Game Boy Pixel Shadow | Game Boy | Off, Up, Up Right, Right, Down Right (default), Down, Down Left, Left, Up Left |
| | LCD Effect Threads | Both | Auto, 1, 2, 3, 4 |
| Audio | Low-Pass Filter | Both | Disabled, 20%, 40%, 60%, 80% |
| Input | Turbo Button Period | Both | 4 to 120 frames, 4 by default |
| | Tilt Sensor | Both | Left Stick, Right Stick, D-Pad |
| | Solar Sensor | Game Boy Advance | 0 (Dark) to 10 (Full Sun), 5 by default |
| | Restart Buttons | Both | L + R + Start, L + R + A + B, L3 + R3, Select + Start + A + B, Off |
| Link | Link Mode (applies on restart) | Both | Off, Together, Separate, GameCube (Game Boy Advance) |
| Link: Players | Players (applies on restart) | Both | 2, 3, 4 (Together) |
| | Player 2's Game (applies on restart) | Both | Same Game, then the games beside player 1's (Together) |
| Link: Connection | This Console's Cartridge (applies on restart) | Game Boy Advance | Inserted, Removed (Separate, GameCube) |
| | Link Port (applies on restart) | Game Boy Advance | Link Cable, Wireless Adapter |
| | GameCube Address | Game Boy Advance | Find on the Network, This Device (GameCube) |
| | Link Port (applies on restart) | Game Boy | Link Cable, Four Player Adapter (Together) |
| | Game Boy Link Timing | Game Boy | Standard, Exact (Separate) |
| Link: Screens and Controls | Screens | Both | Side by Side, Top and Bottom, Player 1 Only to Player 4 Only (Together) |
| | L2 Button | Both | Fast Forward, Previous Screens (Together) |
| | Switch Controller | Both | L + R + A + B, Start + L2 + R2, Off |
| Saves | Back Up Save | Both | Off, then each player |
| | Load Save | Both | Off, then each player's current save (with two or more) and backups |
| | Load Into | Both | Same Player, then each player (Together) |
| | Send Save | Both | Off, then player 1's current save, backups, remote and received saves (Separate) |
| | New Players' Saves (applies on restart) | Both | Create New Save, Copy Player 1's Save (Together) |
| | Player 1's Save Folder (applies when a game starts) | Both | RetroArch's Save Folder, armGBA/Player 1 Folder |
| | Backups Kept | Both | 10, 5, 20, All |
| | Backups Listed | Both | 10, 20, 40, 80, 126 |

On the Game Boy Advance model, a Game Boy game is shown at its own size. With Game Boy Advance
Border set to Shown, the black around it shows too, as on that console's screen. Game Boy Advance
Panel shows its screen as the Advance's panel or the Color's: their colors with Color Correction on,
their LCD Grid and their ghosting. Interleaved Rows, on the Advance's panel with LCD Ghosting on,
drives alternate rows harder each frame, so pictures that alternate each frame show in stripes, as
on the console.

LCD Grid draws the gaps between the console screen's pixels into a picture as many times larger,
with the shadow a first Game Boy's or a Pocket's pixels cast: pick the largest that fits the screen,
with RetroArch's integer scaling on. It works on the CPU, in vectors (NEON, SSE or AVX), its rows
shared out among the cores, as LCD Ghosting's are: LCD Effect Threads says how many, Auto leaving
one core for the rest, or two with Threaded Video. Its shadow is one shade for each pixel's square,
a close likeness of the smooth one the shaders and the standalone frontend draw, at a fraction of
the cost. At 6x, a Game Boy game's grid adds about 1.5 ms a frame on an RG353M and its shadow 0.1 ms
more; on a Core i7-9700F, 0.07 ms and 0.02 ms. On a small screen, such as a 640x480 handheld's, 2x
with integer scaling looks cleanest. `shaders/` holds RetroArch shader presets, `.slangp` and
`.glslp`, that draw the same on the GPU at any size instead, one for each screen: `gb-green`,
`gb-gray`, `gb-pocket` and `gb-light` (with the shadow), `gbc` and `gba`. The standalone frontend
draws its own on the GPU, or the core's. Drawn at any size, the grid shows a tenth of the photos'
darkness for each time larger the picture is, all of it from 10x: small, its gaps would land
unevenly and read too heavy.

## Building

    cmake -S . -B build -G Ninja
    ninja -C build

This makes `armgba_libretro.so`. `libretro/armgba_libretro.info` goes in RetroArch's info directory.

The recompiler has backends for x86-64, AArch64 and 32-bit ARM (ARMv7 with NEON). For a 32-bit ARM
device such as the Miyoo Mini, cross-compile with that device's own toolchain, whose toolchain file
sets `CMAKE_SYSTEM_PROCESSOR` to `arm`, and the ARMv7 flags:

    cmake -S . -B build-armv7 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<toolchain file> \
        -DCMAKE_C_FLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard" \
        -DCMAKE_CXX_FLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard"
    ninja -C build-armv7

On any other CPU, or with `-DARMGBA_JIT=OFF`, the exact CPU runs everything.

For RetroArch on Android, with the NDK, for `arm64-v8a`, `armeabi-v7a` or `x86_64`; the core is
then named `armgba_libretro_android.so`:

    cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
    cmake --build build-android

### Standalone

`armgba`, armGBA's own frontend (`frontend/`), runs a game in a window with SDL2, without RetroArch:

    cmake -S . -B build -G Ninja -DARMGBA_SDL=ON
    ninja -C build armgba-sdl

`appimage/build.sh`, then `appimage/package.sh`, make AppImages for x86-64 and 64-bit ARM in a
Debian 11 container (Docker), with SDL2 built in; they need only glibc 2.29 or later.

Run `armgba game.gba` (or a `.gb`, `.gbc` or `.zip`), or drop a game on the window. Esc, a
controller's Guide button or a right-click opens the menu, which the keyboard, a controller or the
mouse moves through. Arrow keys, X = A, Z = B, A = L, S = R, Enter = Start, Backspace = Select, C and
V turbo B and A; controllers work too, one per player. Tab fast-forwards while held, R rewinds, P
pauses, F2 restarts, F5 and F7 save and load a state, F6 and F8 change its slot, F12 takes a
screenshot, F9 starts and stops recording, F11 goes full screen. The menu changes all of these.

The menu also has recent games, Emulator Options (the core's) and General Settings, cheats
(RetroArch's `.cht` files can be added), ten state slots with Undo Load State, the picture (integer
scaling, vsync, and the core's screen options with its CPU LCD grid), a Shaders page for what the
GPU draws (Sharp, Smooth, the GPU LCD grid and the Game Boy pixel shadow), the sound's volume and
latency, rewind, run-ahead, netplay, RetroAchievements, and the menus' own look: accent colour,
size, and how dark the game is behind them. The line under the menu says what the selected item
does. With a controller, the buttons bound to A and B choose and go back, and L and R jump by letter
in the game list; on the keyboard, typing a name's start finds it. Restart, Close Game, Quit and
Reset Controls and Hotkeys ask for a second press. Netplay is armgba to armgba: Separate carries the
link cable between two devices each running its own game; Together runs every player's console on
every device, kept in step with rollback. `--host`, `--host-together` and `--connect ADDRESS` start
it from the command line. Achievements need the system's libcurl, and recording its ffmpeg.

The frontend's settings are in `~/.config/armgba/frontend.cfg`, the core's options in
`~/.config/armgba/armgba.cfg` and the recent games in `~/.config/armgba/recent.txt`; BIOS and boot
ROM files go in `~/.config/armgba/system`, saves, states, screenshots and cheats in
`~/.local/share/armgba`.

Game Boy Advance games run on Nintendo's `gba_bios.bin` from the system directory when it's there
and BIOS is set to Nintendo's, and otherwise on armGBA's own open-source BIOS, built into the core.
Its splash lasts as long as Nintendo's logo, and a game starts on the same cycle.

Game Boy boot ROMs are optional, looked for under these names in this order:

- Game Boy: `gb_bios.bin`, `dmg_boot.bin`
- Game Boy Pocket: `mgb_boot.bin`
- Game Boy Color: `gbc_bios.bin`, `cgb_boot.bin`
- Game Boy Advance: `cgb_agb_boot.bin`, `cgb_agb0_boot.bin`, `agb_boot.bin`, then the Game Boy
  Color's (converted)

With Boot ROM set to Nintendo's, the boot ROM is used under whichever of these names it has. A
SameBoy boot ROM under one of them is used only where Nintendo's isn't there. With neither, armGBA's
own starts the game.

armGBA's own boot ROMs (`gbboot/`, built into the core) are the size of Nintendo's and run on the
hardware: on the Game Boy and the Pocket, in 256 bytes, the cartridge's logo scrolls down as on
Nintendo's; on the Game Boy Color and Game Boy Advance, in 2,304, "ARMGBC" comes in over the
cartridge's logo, as Nintendo's title does. They last as long as Nintendo's, to the cycle, and leave
the machine as Nintendo's do, and give original Game Boy games on a Game Boy Color the same colours,
chosen the same ways. These were found from what Nintendo's boot ROMs leave and show
(`tests/gbcheck/`), never from their code.

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
families. The screen palettes, apart from Grayscale (gambatte-libretro's), are SameBoy's. So is the
Game Boy Color and Game Boy Advance color correction. The boot ROMs built in are armGBA's own.

**Link play.** Link Mode picks how consoles link. Off, the default, is one console, with RetroArch's
usual netplay.

| You want to… | Use |
| --- | --- |
| Play alone | Link Mode Off |
| Play together on one device, or smooth netplay | Together |
| Link with someone who has only their own game, or whose device is too slow for two consoles | Separate |
| Trade or battle in Pokemon FireRed, LeafGreen or Emerald over Wi-Fi | Separate with the Wireless Adapter |
| Play Kirby & The Amazing Mirror or another fast-linking game online | Together |
| Fix a Game Boy game that errors in Separate | Game Boy Link Timing: Exact |
| Be a Game Boy Advance plugged into a GameCube game in Dolphin | GameCube |

The two ways to link:

- Separate: each device runs its own player's console, with their own game and save, and the cable
  crosses RetroArch's netplay. Players can run different games, such as Red and Blue, or Yellow and
  Crystal. It links two players; three or four play Together. Each transfer waits for the other
  end, so a slow network stalls the cable. Game Boy Advances keep to the same cycles, as on a real
  cable, which games that link every frame, such as Kirby & The Amazing Mirror, need; it wants a
  short ping. Game Boys run at their own pace, the one waiting sending its next byte ahead, which
  suits Wi-Fi and games that link in turns; Game Boy Link Timing set to Exact times them as Game
  Boy Advances are. The Wireless Adapter needs neither: each device runs freely.
- Together: every device runs every player's console, two to four with the Players option, for
  multiplayer on one device (a controller each) or with RetroArch's rollback netplay, where the
  link never waits on the network. Every device needs every game. Three or four are shown in a
  grid. Player 2's game is picked with Player 2's Game; players 3 and 4 play player 1's. A player on
  player 1's game gets a save of their own, under the game's file name in the save folder's
  armGBA/Player 2 to Player 4 folders; it starts new, or as a copy of player 1's, as New Players'
  Saves says. In netplay the session runs the host's games and saves, and the host writes the
  players' saves as the game closes. A device that joined keeps its own saves as they were, and
  the session's in the save folder's armGBA/Remote folders (see Backups).

**Backups.**

- Back Up Save copies a player's save, as it is, to a file named for the time, such as
  `Game.20261018-153012.srm`, beside their save; player 1's goes in the save folder's
  armGBA/Player 1 folder. The game carries on.
- Load Save makes a backup a player's save again. With two players or more it can also give one
  player another's current save or backup, as Load Into says. It backs up the save it replaces
  first and restarts that player's console. Not during netplay.
- A device that joined another's netplay session keeps that session's saves in armGBA/Remote/Player
  1 to Player 4, by the session's players, and Load Save lists them as Remote Player 1 to 4. Each
  goes to the other person unless Load Into names a player: Remote Player 1 (the host) to player 2
  here when there is one, Remote Player 2 to player 1. Hosting and joining in turn passes both
  players' saves each way.
- In Separate, linked devices swap their current saves as they connect, and Send Save sends the
  current save, a backup, or a remote or received save, when chosen. The other device keeps it in
  armGBA/Received, and Load Save lists it as Received, going to player 2 when there is one, or
  player 1. This is the way to bring a player's own save to the host before playing Together.
- Backups Kept: 10 per player unless chosen. A save the same as its newest backup isn't backed up
  again.
- Player 1's Save Folder can keep player 1's save in armGBA/Player 1 too, beside the other players'.
  When it changes, the newer save is used.

The Game Boy Advance Wireless Adapter (Link Port) works with either: the Union Room and trades of
Pokemon FireRed, LeafGreen and Emerald, and other games using Nintendo's wireless library. The Game
Boy Four Player Adapter works with Together, and three or four Game Boys always use it: Wario Blast,
F-1 Race, Yoshi's Cookie and other four-player games.

**GameCube.** With Link Mode on GameCube, this Game Boy Advance is plugged into a controller port of
Dolphin set to GBA (TCP), on the same device or another on the local network, and plays its part in
the GameCube game, as Four Swords Adventures' players do. For a program the GameCube sends, set
This Console's Cartridge to Removed; either BIOS takes it. GameCube Address finds Dolphin by itself;
This Device looks only here. The console keeps to Dolphin's time while the game talks to it, so the
two run in step.

**Pair files.** A `.gb2` file, as Batocera writes them, or a `.gba2` file names the games of a link
played Together, two to four, one per line, and opens like a game from RetroArch's Load Content:

    gb:Pokemon - Yellow Version.zip
    gbc:Pokemon - Crystal Version.zip

`gb:`, `gbc:` and `gba:` mean that folder beside the pair file's folder; a line can also be a path
from the pair file's folder, or a full path. Each player keeps their game's own save, where RetroArch
keeps that game's save.

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

- **Lockstep:** twenty games compared frame by frame with the stepping CPU on all three backends, and
  every game of the Game Boy Advance set (4590) for 1500 frames on x86-64.
- **`tests/cpufuzz`:** random ARM and Thumb programs against mGBA.
- **Hardware test ROMs:** all 28 pass (jsmolka's gba-tests, FuzzARM, NanoBoyAdvance's).
- **mGBA test suite:** every scored category passes in full except Misc edge, at 6 of 12.
- **Game Boy test ROMs:** 213 of 236 screens match a SameBoy-based build. The rest are SameSuite's
  APU tests, which time the sound channels to a fraction of a CPU cycle.
- **Game Boy games:** every game of the Game Boy and Game Boy Color sets was started and compared with
  SameBoy: 2776 of 2796 and 2983 of 3055 match. The rest are not games (BIOS files, cheat cartridges),
  betas, and unlicensed carts that differ on the other cores too.
- **Save states and runahead:** frame-exact on x86-64 and AArch64 for GBA, GBC and GB. A Game Boy state
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

**Recompiler with an exact fallback.** ARM and Thumb code is compiled to AArch64, 32-bit ARM or
x86-64 and counts its own cycles. When an event (timer, video line, DMA, IRQ) falls inside a block,
a copy of the block with a check before each instruction takes over. An event the instruction cannot
observe runs after it. For one it can observe, a cycle-exact stepping CPU (`src/cpu/step.c`) runs that
single instruction.

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
- Four Game Boy Advances on one device need a fast one with four cores: each runs on a core of its
  own, and while they link they wait for each other often.
- The frontend's netplay connects only to another armgba, not to RetroArch.
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
- **SameBoy** (Lior Halphon, MIT): the Game Boy screen palettes other than Grayscale, and the Game
  Boy Color and Game Boy Advance color correction curves. A SameBoy-based build is also the
  reference the Game Boy side is tested against.
- **libretro** (MIT): `libretro/libretro.h`.
- **RetroArch** (MIT, public domain): libretro-common, the LZMA SDK and rcheevos as RetroArch
  carries them, in `libretro/deps/` (see its README).
- **stb_truetype** (Sean Barrett, public domain or MIT): the frontend's text.
- **Inter** (The Inter Project Authors) and **Chakra Petch** (The Chakra Petch Project Authors),
  OFL-1.1: the frontend's fonts.

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
Code). Most of the code, comments and documentation were drafted by it at Jacob Cook's direction.
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
| `libretro/deps/` | MIT and public domain, as each file states |
| `frontend/`, `appimage/`, `shaders/`, `bios/`, `gbboot/` | Mozilla Public License 2.0 |
| `frontend/stb_truetype.h` | public domain or MIT |
| `frontend/fonts/` | SIL Open Font License 1.1 (`LICENSES/OFL-1.1.txt`) |
| The Game Boy palettes (other than Grayscale) and color curves in `libretro/gb-gambatte.cpp` | MIT, from SameBoy (Lior Halphon) |
