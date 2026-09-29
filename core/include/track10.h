/*
 * Track 10 core: plays Native Instruments stem files (.stem.mp4) with a
 * separate volume for each stem.
 *
 * The frontends own no audio code. They create a player, open a file, and
 * call t10_player_poll() from a UI timer, which is also where they refresh
 * the position. Every function here must be called from that same thread.
 */
#ifndef TRACK10_H
#define TRACK10_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T10_MAX_STEMS 8

typedef struct T10Player T10Player;

/* Bits returned by t10_player_poll(). */
enum {
    T10_EVENT_EOS = 1 << 0,   /* reached the end, now paused at the start */
    T10_EVENT_ERROR = 1 << 1, /* playback failed, see t10_player_error() */
};

/*
 * Creates a player and opens `path`, blocking until the audio is ready to
 * play (normally well under a second). Returns NULL on failure and, when
 * `error` is not NULL, sets it to a message the caller frees with free().
 */
T10Player *t10_player_open(const char *path, char **error);
void t10_player_free(T10Player *player);

/* Handles pending pipeline messages and returns T10_EVENT_* bits. */
unsigned t10_player_poll(T10Player *player);
const char *t10_player_error(const T10Player *player);

void t10_player_play(T10Player *player);
void t10_player_pause(T10Player *player);
bool t10_player_is_playing(const T10Player *player);

/* Seconds. Seeking clamps to the track's bounds. */
double t10_player_position(T10Player *player);
double t10_player_duration(const T10Player *player);
void t10_player_seek(T10Player *player, double seconds);

/* Playback speed, pitch preserved. 1.0 is normal speed. */
double t10_player_rate(const T10Player *player);
void t10_player_set_rate(T10Player *player, double rate);

/* Stems, in file order: Drums, Bass, Other, Vocals for a standard file. */
int t10_player_stem_count(const T10Player *player);
const char *t10_player_stem_name(const T10Player *player, int stem);
/* 0xRRGGBB, as stored in the file. */
uint32_t t10_player_stem_color(const T10Player *player, int stem);
/* Linear gain, 0.0 to 1.0. */
double t10_player_stem_volume(const T10Player *player, int stem);
void t10_player_set_stem_volume(T10Player *player, int stem, double volume);

/* Tags from the file, or NULL when the file does not have them. */
const char *t10_player_title(const T10Player *player);
const char *t10_player_artist(const T10Player *player);
const char *t10_player_album(const T10Player *player);

/* Cover art as encoded image bytes (JPEG or PNG), or NULL. */
const uint8_t *t10_player_cover(const T10Player *player, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
