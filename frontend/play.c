/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Save states, rewind, run-ahead, screenshots and recording.

#include "frontend.h"
#include "games.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

// Save states: slot 0 is game.state, the others game.state1 to game.state99, as RetroArch names
// them; slot -1 is game.state.auto, saved on exit and loaded on start when asked.

static void statePath(char out[FILE_SIZE], int slot) {
	if (slot < 0) {
		snprintf(out, FILE_SIZE, "%s/%s.state.auto", stateDirectory, app.gameName);
	} else if (slot == 0) {
		snprintf(out, FILE_SIZE, "%s/%s.state", stateDirectory, app.gameName);
	} else {
		snprintf(out, FILE_SIZE, "%s/%s.state%d", stateDirectory, app.gameName, slot);
	}
}

bool stateExists(int slot) {
	char path[FILE_SIZE];
	statePath(path, slot);
	struct stat st;
	return app.loaded && !stat(path, &st);
}

static bool writePng(const char* path);

// The picture saved with slot's state, game.state.png and so on, as RetroArch names it.
void stateThumbnailPath(char out[FILE_SIZE + 8], int slot) {
	char path[FILE_SIZE];
	statePath(path, slot);
	snprintf(out, FILE_SIZE + 8, "%s.png", path);
}

// How many states have been saved, so a thumbnail shown is read again after one is.
int statesSaved;

// The game's state now, to slot's file, with the picture beside it.
bool saveState(int slot) {
	size_t const size = retro_serialize_size();
	void* data = size ? malloc(size) : NULL;
	char path[FILE_SIZE];
	statePath(path, slot);
	bool const saved = data && retro_serialize(data, size) && writeWholeFile(path, data, size);
	free(data);
	if (saved) {
		char thumbnail[FILE_SIZE + 8];
		stateThumbnailPath(thumbnail, slot);
		if (!writePng(thumbnail)) {
			remove(thumbnail);
		}
		++statesSaved;
	}
	return saved;
}

// The game as it was before the last state loaded, for Undo Load State.
static void* undoData;
static size_t undoSize;

// Whether there is a game to go back to.
bool canUndoLoad(void) {
	return undoData != NULL;
}

// A state that stays in this session: the core may save it exactly, as it needn't load anywhere else.
static bool keepState(void* data, size_t size) {
	app.stateContext = RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE;
	bool const saved = retro_serialize(data, size);
	app.stateContext = RETRO_SAVESTATE_CONTEXT_NORMAL;
	return saved;
}

static bool restoreState(const void* data, size_t size) {
	app.stateContext = RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE;
	bool const loaded = retro_unserialize(data, size);
	app.stateContext = RETRO_SAVESTATE_CONTEXT_NORMAL;
	return loaded;
}

void undoForget(void) {
	free(undoData);
	undoData = NULL;
	undoSize = 0;
}

// Back to the game as it was before the last state loaded; that can be undone only once.
bool undoLoadState(void) {
	if (!undoData || !restoreState(undoData, undoSize)) {
		return false;
	}
	undoForget();
	rewindReset();
	cheevosReset();
	return true;
}

// Save State and Load State, from the menu or a hotkey, for the slot chosen. Not in hardcore mode,
// and not during netplay, where every device must run the same game.
bool stateCommand(bool load) {
	if (cheevosHardcore()) {
		osd("Hardcore mode: save states are off");
		return false;
	}
	if (netRunning()) {
		osd(load ? "States can't be loaded during netplay" : "States can't be saved during netplay");
		return false;
	}
	bool done;
	if (!load) {
		done = saveState(app.slot);
		osd(done ? "Saved state %d" : "Couldn't save state %d", app.slot);
	} else if (!stateExists(app.slot)) {
		done = false;
		osd("State %d is empty", app.slot);
	} else {
		done = loadState(app.slot);
		osd(done ? "Loaded state %d" : "State %d didn't load", app.slot);
	}
	return done;
}

// Loads slot's state, keeping the game as it was for Undo Load State.
bool loadState(int slot) {
	char path[FILE_SIZE];
	statePath(path, slot);
	size_t size;
	void* data = gameRead(path, &size);
	size_t const keptSize = data ? retro_serialize_size() : 0;
	void* kept = keptSize ? malloc(keptSize) : NULL;
	if (kept && !keepState(kept, keptSize)) {
		free(kept);
		kept = NULL;
	}
	bool const loaded = data && retro_unserialize(data, size);
	free(data);
	if (loaded) {
		undoForget();
		undoData = kept;
		undoSize = keptSize;
		rewindReset();
		cheevosReset();
	} else {
		free(kept);
	}
	return loaded;
}

// Rewind: every frame's state is kept as how it differs from the one after it, the two XORed and
// the zero words between the rest counted, in a ring of the size asked for; the oldest go first
// when it fills. Going back undoes the newest difference on the state now, shows that frame and
// goes back to its start.

struct Entry {
	size_t at, size;
};

static uint8_t* ring;
static size_t ringSize, ringAt;
static struct Entry* entries;
static size_t entryRoom, entryFirst, entryCount;
static uint32_t* stateNow;
static uint32_t* stateNew;
static size_t stateWords;
static uint32_t* packed;

// Starts rewinding afresh, with what was kept forgotten, as the game, a state or the setting changes.
// Without the memory, rewind is turned off.
void rewindReset(void) {
	free(ring);
	free(entries);
	free(stateNow);
	free(stateNew);
	free(packed);
	ring = NULL;
	entries = NULL;
	stateNow = stateNew = packed = NULL;
	entryCount = entryFirst = ringAt = 0;
	if (!settings.rewind || !app.loaded) {
		return;
	}
	size_t const size = retro_serialize_size();
	stateWords = (size + 3) / 4;
	ringSize = (size_t) settings.rewindMegabytes << 20;
	entryRoom = 1 << 16;
	ring = malloc(ringSize);
	entries = calloc(entryRoom, sizeof(*entries));
	stateNow = calloc(stateWords, 4);
	stateNew = calloc(stateWords, 4);
	// At worst, every word differs: a count pair for each and the words.
	packed = malloc(stateWords * 12 + 16);
	if (!ring || !entries || !stateNow || !stateNew || !packed || !keepState(stateNow, size)) {
		settings.rewind = false;
		rewindReset();
		osd("Not enough memory to rewind");
	}
}

// a XOR b as runs: a count of zero words, a count of words that follow, those words; then again.
static size_t pack(const uint32_t* a, const uint32_t* b, size_t words, uint32_t* out) {
	size_t o = 0, i = 0;
	while (i < words) {
		size_t zeros = 0;
		while (i < words && a[i] == b[i]) {
			++zeros;
			++i;
		}
		size_t const start = i;
		while (i < words && (a[i] != b[i] || (i + 1 < words && a[i + 1] != b[i + 1]))) {
			++i;
		}
		out[o++] = (uint32_t) zeros;
		out[o++] = (uint32_t) (i - start);
		for (size_t k = start; k < i; ++k) {
			out[o++] = a[k] ^ b[k];
		}
	}
	return o * 4;
}

// XORs what pack made onto state, which takes it back to the state packed against.
static void unpack(const uint32_t* in, size_t bytes, uint32_t* state, size_t words) {
	size_t const n = bytes / 4;
	size_t i = 0, w = 0;
	while (i + 1 < n && w <= words) {
		w += in[i++];
		size_t const count = in[i++];
		for (size_t k = 0; k < count && w < words && i < n; ++k) {
			state[w++] ^= in[i++];
		}
	}
}

// Keeps the frame just run: how the state before it differs. A difference larger than a quarter of
// the ring isn't kept, so rewinding stops there.
void rewindPush(void) {
	if (!ring) {
		return;
	}
	size_t const size = retro_serialize_size();
	if ((size + 3) / 4 != stateWords || !keepState(stateNew, size)) {
		rewindReset();
		return;
	}
	size_t const bytes = pack(stateNow, stateNew, stateWords, packed);
	if (bytes > ringSize / 4) {
		memcpy(stateNow, stateNew, stateWords * 4);
		entryCount = 0;
		return;
	}
	if (ringAt + bytes > ringSize) {
		ringAt = 0;
	}
	// The oldest entries that this one overwrites go, and the oldest if there's no room for another.
	while (entryCount) {
		struct Entry* oldest = &entries[entryFirst];
		bool const overlaps = oldest->at < ringAt + bytes && ringAt < oldest->at + oldest->size;
		if (!overlaps && entryCount < entryRoom) {
			break;
		}
		entryFirst = (entryFirst + 1) % entryRoom;
		--entryCount;
	}
	memcpy(ring + ringAt, packed, bytes);
	entries[(entryFirst + entryCount) % entryRoom] = (struct Entry) { ringAt, bytes };
	++entryCount;
	ringAt += bytes;
	uint32_t* t = stateNow;
	stateNow = stateNew;
	stateNew = t;
}

// Goes back one frame; false when there's nothing more to go back to.
bool rewindStep(void) {
	if (!ring || !entryCount) {
		return false;
	}
	--entryCount;
	struct Entry const e = entries[(entryFirst + entryCount) % entryRoom];
	unpack((const uint32_t*) (ring + e.at), e.size, stateNow, stateWords);
	ringAt = e.at;
	size_t const size = retro_serialize_size();
	restoreState(stateNow, size);
	// The frame shows, without its sound, and the game goes back to its start.
	int const av = app.avEnable;
	app.avEnable = RETRO_AV_ENABLE_VIDEO;
	retro_run();
	app.avEnable = av;
	restoreState(stateNow, size);
	return true;
}

// Run-ahead, as RetroArch does it with one core: the frame runs with its sound and no picture and
// its state is kept; the frames ahead run unseen and unheard, the last of them shown; then the
// kept state comes back, so the game goes on from the real frame.
void runAheadFrame(void) {
	static void* kept;
	static size_t keptSize;
	size_t const size = retro_serialize_size();
	if (size != keptSize) {
		free(kept);
		kept = malloc(size);
		keptSize = kept ? size : 0;
	}
	int const av = app.avEnable;
	app.avEnable = RETRO_AV_ENABLE_AUDIO;
	retro_run();
	if (!kept || !keepState(kept, size)) {
		app.avEnable = av;
		return;
	}
	for (int i = 1; i < settings.runahead; ++i) {
		app.avEnable = 0;
		retro_run();
	}
	app.avEnable = RETRO_AV_ENABLE_VIDEO;
	retro_run();
	restoreState(kept, size);
	app.avEnable = av;
}

// The picture as 8-bit RGB, from the core's RGB565 or XRGB8888.
static uint8_t* pictureRgb(void) {
	int const w = app.gameWidth, h = app.gameHeight;
	uint8_t* rgb = malloc((size_t) w * h * 3);
	if (!rgb) {
		return NULL;
	}
	for (int y = 0; y < h; ++y) {
		const uint8_t* row = app.picture + y * app.picturePitch;
		for (int x = 0; x < w; ++x) {
			uint8_t* out = &rgb[(y * w + x) * 3];
			if (app.format == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t const c = ((const uint32_t*) row)[x];
				out[0] = c >> 16;
				out[1] = c >> 8;
				out[2] = c;
			} else {
				uint16_t const c = ((const uint16_t*) row)[x];
				out[0] = (c >> 11) << 3 | (c >> 13);
				out[1] = ((c >> 5) & 0x3F) << 2 | ((c >> 9) & 3);
				out[2] = (c & 0x1F) << 3 | ((c >> 2) & 7);
			}
		}
	}
	return rgb;
}

// A PNG chunk: its length, type, data and CRC.
static void pngChunk(FILE* f, const char* type, const uint8_t* data, uint32_t size) {
	uint8_t head[8] = { size >> 24, size >> 16, size >> 8, size, type[0], type[1], type[2], type[3] };
	fwrite(head, 1, 8, f);
	fwrite(data, 1, size, f);
	// zlib's crc32 gives back its starting value, not the one passed in, for no data (IEND's).
	uLong crc = crc32(0, head + 4, 4);
	if (size) {
		crc = crc32(crc, data, size);
	}
	uint8_t tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
	fwrite(tail, 1, 4, f);
}

// The local time, for file names.
static void timestamp(char* out, size_t size) {
	time_t const now = time(NULL);
	struct tm t;
	localtime_r(&now, &t);
	strftime(out, size, "%Y%m%d-%H%M%S", &t);
}

// The picture as the core made it, unscaled, to a PNG at path.
static bool writePng(const char* path) {
	if (!app.picture || !app.gameWidth) {
		return false;
	}
	int const w = app.gameWidth, h = app.gameHeight;
	uint8_t* rgb = pictureRgb();
	// Each row with filter type 0 before it.
	size_t const raw = (size_t) (w * 3 + 1) * h;
	uint8_t* rows = malloc(raw);
	uLongf compressedSize = compressBound(raw);
	uint8_t* compressed = malloc(compressedSize);
	FILE* f = rgb && rows && compressed ? fopen(path, "wb") : NULL;
	if (f) {
		for (int y = 0; y < h; ++y) {
			rows[y * (w * 3 + 1)] = 0;
			memcpy(&rows[y * (w * 3 + 1) + 1], &rgb[y * w * 3], w * 3);
		}
		compress2(compressed, &compressedSize, rows, raw, 9);
		static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		uint8_t header[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
		fwrite(signature, 1, 8, f);
		pngChunk(f, "IHDR", header, 13);
		pngChunk(f, "IDAT", compressed, (uint32_t) compressedSize);
		pngChunk(f, "IEND", NULL, 0);
	}
	bool const written = f && !ferror(f);
	if (f) {
		fclose(f);
	}
	free(rgb);
	free(rows);
	free(compressed);
	return written;
}

// An 8-bit RGB or RGBA PNG, not interlaced, as RGB rows; NULL if it's another kind or can't be read.
uint8_t* readPng(const char* path, int* width, int* height) {
	size_t size;
	uint8_t* file = gameRead(path, &size);
	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	if (!file || size < 33 || memcmp(file, signature, 8)) {
		free(file);
		return NULL;
	}
	uint32_t w = 0, h = 0;
	int channels = 0;
	uint8_t* packed = NULL;
	size_t packedSize = 0;
	bool ok = true;
	for (size_t at = 8; ok && at + 12 <= size;) {
		uint32_t const length = (uint32_t) file[at] << 24 | file[at + 1] << 16 | file[at + 2] << 8 | file[at + 3];
		const uint8_t* const type = &file[at + 4];
		const uint8_t* const data = &file[at + 8];
		if (length > size - at - 12) {
			ok = false;
		} else if (!memcmp(type, "IHDR", 4) && length >= 13) {
			w = (uint32_t) data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
			h = (uint32_t) data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
			channels = data[9] == 2 ? 3 : data[9] == 6 ? 4 : 0;
			ok = data[8] == 8 && channels && !data[12] && w && h && w <= 4096 && h <= 4096;
		} else if (!memcmp(type, "IDAT", 4)) {
			uint8_t* const grown = realloc(packed, packedSize + length);
			ok = grown != NULL;
			if (ok) {
				packed = grown;
				memcpy(packed + packedSize, data, length);
				packedSize += length;
			}
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		at += 12 + (size_t) length;
	}
	free(file);
	size_t const stride = (size_t) w * channels, rawSize = (stride + 1) * h;
	uint8_t* const raw = ok && packed && channels ? malloc(rawSize) : NULL;
	uLongf unpacked = rawSize;
	ok = raw && uncompress(raw, &unpacked, packed, packedSize) == Z_OK && unpacked == rawSize;
	free(packed);
	uint8_t* const rgb = ok ? malloc((size_t) w * h * 3) : NULL;
	if (!rgb) {
		free(raw);
		return NULL;
	}
	// Each row's filter undone against the row above, already undone.
	for (uint32_t y = 0; y < h; ++y) {
		uint8_t* const row = raw + y * (stride + 1) + 1;
		const uint8_t* const above = y ? row - (stride + 1) : NULL;
		int const filter = row[-1];
		for (size_t x = 0; x < stride; ++x) {
			int const a = x >= (size_t) channels ? row[x - channels] : 0, b = above ? above[x] : 0;
			int const c = above && x >= (size_t) channels ? above[x - channels] : 0;
			int const p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
			int const paeth = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
			int const add = filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2 : filter == 4 ? paeth : 0;
			row[x] = (uint8_t) (row[x] + add);
		}
		for (uint32_t x = 0; x < w; ++x) {
			memcpy(&rgb[(y * w + x) * 3], &row[x * channels], 3);
		}
	}
	free(raw);
	*width = (int) w;
	*height = (int) h;
	return rgb;
}

// The picture as the core made it, unscaled, to a PNG in the screenshots folder.
void takeScreenshot(void) {
	if (!app.picture || !app.gameWidth) {
		return;
	}
	char stamp[32], path[FILE_SIZE];
	timestamp(stamp, sizeof(stamp));
	snprintf(path, sizeof(path), "%s/%s-%s.png", shotDirectory, app.gameName, stamp);
	if (writePng(path)) {
		osd("Screenshot: %s", path);
	} else {
		osd("Can't write the screenshot");
	}
}

// Recording: the picture goes to ffmpeg as it comes, the sound to a WAV beside it; stopping, ffmpeg
// joins them into an MP4 in the screenshots folder.

static FILE* videoPipe;
static FILE* soundFile;
static int recordWidth, recordHeight;
static uint32_t soundBytes;
static char videoPath[FILE_SIZE], soundPath[FILE_SIZE], moviePath[FILE_SIZE];

// Whether recording is on.
bool recording(void) {
	return videoPipe != NULL;
}

// Whether ffmpeg is on the PATH.
static bool hasFfmpeg(void) {
	const char* path = getenv("PATH");
	if (!path) {
		return false;
	}
	char directories[PATH_SIZE];
	snprintf(directories, sizeof(directories), "%s", path);
	for (char* d = strtok(directories, ":"); d; d = strtok(NULL, ":")) {
		char candidate[FILE_SIZE];
		snprintf(candidate, sizeof(candidate), "%s/ffmpeg", d);
		if (!access(candidate, X_OK)) {
			return true;
		}
	}
	return false;
}

// path as one word for the shell, whatever the game's name holds: in single quotes, each quote in it
// ending them, escaped and starting them again.
static void shellWord(char* out, size_t size, const char* path) {
	size_t o = 0;
	out[o++] = '\'';
	for (const char* p = path; *p && o + 6 < size; ++p) {
		if (*p == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *p;
		}
	}
	out[o++] = '\'';
	out[o] = '\0';
}

// The WAV file's header for bytes of 16-bit stereo, written over the first 44 bytes.
static void wavHeader(FILE* f, uint32_t bytes) {
	uint32_t const rate = AUDIO_RATE;
	uint8_t h[44] = { 'R', 'I', 'F', 'F' };
	uint32_t const riff = 36 + bytes;
	memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	uint32_t const fmtSize = 16;
	uint16_t const pcm = 1, channels = 2, block = 4, bits = 16;
	uint32_t const byteRate = rate * 4;
	memcpy(h + 16, &fmtSize, 4);
	memcpy(h + 20, &pcm, 2);
	memcpy(h + 22, &channels, 2);
	memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byteRate, 4);
	memcpy(h + 32, &block, 2);
	memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &bytes, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
}

// Starts recording, or stops it; true if it started.
bool recordToggle(void) {
	if (recording()) {
		recordStop();
		return false;
	}
	if (!app.loaded || !app.gameWidth) {
		return false;
	}
	if (!hasFfmpeg()) {
		osd("Recording needs ffmpeg installed");
		return false;
	}
	char stamp[32];
	timestamp(stamp, sizeof(stamp));
	snprintf(moviePath, sizeof(moviePath), "%s/%s-%s.mp4", shotDirectory, app.gameName, stamp);
	snprintf(videoPath, sizeof(videoPath), "%s/.recording-video.mkv", shotDirectory);
	snprintf(soundPath, sizeof(soundPath), "%s/.recording-sound.wav", shotDirectory);
	recordWidth = app.gameWidth;
	recordHeight = app.gameHeight;
	static char video[FILE_SIZE * 4], command[FILE_SIZE * 5];
	shellWord(video, sizeof(video), videoPath);
	snprintf(command, sizeof(command),
	         "ffmpeg -loglevel error -y -f rawvideo -pixel_format %s -video_size %dx%d -framerate 59.7275 -i - "
	         "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p %s",
	         app.format == RETRO_PIXEL_FORMAT_XRGB8888 ? "bgra" : "rgb565le", recordWidth, recordHeight, video);
	videoPipe = popen(command, "w");
	soundFile = fopen(soundPath, "wb");
	if (!videoPipe || !soundFile) {
		if (videoPipe) {
			pclose(videoPipe);
		}
		if (soundFile) {
			fclose(soundFile);
		}
		videoPipe = NULL;
		soundFile = NULL;
		osd("Can't start recording");
		return false;
	}
	soundBytes = 0;
	wavHeader(soundFile, 0);
	osd("Recording");
	return true;
}

void recordVideo(const void* data, unsigned width, unsigned height, size_t pitch) {
	// A change of picture size (the linked consoles' screens) isn't recorded.
	if ((int) width != recordWidth || (int) height != recordHeight) {
		return;
	}
	size_t const row = width * (app.format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2);
	for (unsigned y = 0; y < height; ++y) {
		fwrite((const uint8_t*) data + y * pitch, 1, row, videoPipe);
	}
}

// The frame's sound, as it's sent to be heard.
void recordAudio(const int16_t* data, size_t frames) {
	if (soundFile) {
		soundBytes += (uint32_t) fwrite(data, 4, frames, soundFile) * 4;
	}
}

// Stops recording, then joins the picture and sound into the MP4 and deletes them.
void recordStop(void) {
	if (!recording()) {
		return;
	}
	pclose(videoPipe);
	videoPipe = NULL;
	wavHeader(soundFile, soundBytes);
	fclose(soundFile);
	soundFile = NULL;
	static char video[FILE_SIZE * 4], sound[FILE_SIZE * 4], movie[FILE_SIZE * 4], command[FILE_SIZE * 13];
	shellWord(video, sizeof(video), videoPath);
	shellWord(sound, sizeof(sound), soundPath);
	shellWord(movie, sizeof(movie), moviePath);
	snprintf(command, sizeof(command), "ffmpeg -loglevel error -y -i %s -i %s -c:v copy -c:a aac -b:a 160k -shortest %s", video, sound, movie);
	int const result = system(command);
	remove(videoPath);
	remove(soundPath);
	osd(result == 0 ? "Recorded %s" : "Recording failed: %s", moviePath);
}
