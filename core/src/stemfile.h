/* Reads the parts of a stem file GStreamer does not expose. */
#ifndef TRACK10_STEMFILE_H
#define TRACK10_STEMFILE_H

#include "track10.h"

typedef struct {
    char *name;
    uint32_t color;
} T10Stem;

typedef struct {
    int audio_tracks;
    int stem_count;
    T10Stem stems[T10_MAX_STEMS];
    char *title;
    char *artist;
    char *album;
    uint8_t *cover;
    size_t cover_size;
} T10StemFile;

/* On failure returns false and sets `error` to a malloc'd message. */
bool t10_stem_file_read(const char *path, T10StemFile *file, char **error);
void t10_stem_file_clear(T10StemFile *file);

#endif
