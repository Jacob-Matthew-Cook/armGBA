# Archive reading for player 2's game

Player 2's game is read by the core itself, so it uses RetroArch's own archive code to accept the
same `.zip` and `.7z` files RetroArch does for player 1.

- `libretro-common/`: the files RetroArch's archive reading needs, from RetroArch
  bdba046fa6766380bc2457532f38e589df769aaf (v1.22.2 and fixes, the version ROCKNIX ships). MIT
  license, stated in each file. Its `libretro.h` is newer than armGBA's and is used only by this code.
- `7zip/`: the LZMA SDK's decoder, from RetroArch's `deps/7zip` at the same commit. Public domain.

The files are unchanged. Zip support uses the system's zlib.

# Achievements for the standalone frontend

- `rcheevos/`: RetroAchievements' library, v12.3.0 as RetroArch carries it in `deps/rcheevos` at the
  same commit, including RetroArch's `src/rhash/md5.h`, which uses libretro-common's MD5. MIT license.
- `libretro-common/utils/md5.c`, `include/lrc_hash.h` and `include/compat/msvc.h`: that MD5, from the
  same commit. Public domain and MIT.

These files are unchanged too.
