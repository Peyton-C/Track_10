/*
 * The stem names and colours live at moov/udta/stem as raw JSON, and the
 * tags in the iTunes list at moov/udta/meta/ilst. Only moov is read, which
 * sits after the audio in files from bejeweled and NI's tools, so opening
 * a file costs a few hundred KB of reads whatever its size.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "stemfile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* moov is metadata and sample tables, a few MB at the most. */
#define MAX_MOOV_SIZE (64u * 1024 * 1024)

static const char *const default_names[] = {"Drums", "Bass", "Other", "Vocals"};
static const uint32_t default_colors[] = {0xE69F00, 0x56B4E9, 0xCC79A7, 0x009E73};

static char *format_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *message = malloc((size_t)len + 1);
    if (message) {
        va_start(args, fmt);
        vsnprintf(message, (size_t)len + 1, fmt, args);
        va_end(args);
    }
    return message;
}

static char *copy_string(const uint8_t *bytes, size_t len)
{
    char *s = malloc(len + 1);
    if (s) {
        memcpy(s, bytes, len);
        s[len] = '\0';
    }
    return s;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) << 32 | read_u32(p + 4);
}

/* --- Boxes in memory ---------------------------------------------------- */

typedef struct {
    const uint8_t *data; /* payload, after the header */
    size_t size;         /* payload size */
    char type[4];
} Box;

typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
} BoxIter;

static BoxIter box_children(const uint8_t *data, size_t size)
{
    return (BoxIter){data, data + size};
}

static bool box_next(BoxIter *it, Box *box)
{
    size_t left = (size_t)(it->end - it->pos);
    if (left < 8)
        return false;
    uint64_t size = read_u32(it->pos);
    size_t header = 8;
    if (size == 1) {
        if (left < 16)
            return false;
        size = read_u64(it->pos + 8);
        header = 16;
    } else if (size == 0) {
        size = left;
    }
    if (size < header || size > left)
        return false;
    memcpy(box->type, it->pos + 4, 4);
    box->data = it->pos + header;
    box->size = (size_t)size - header;
    it->pos += size;
    return true;
}

static bool box_find(const uint8_t *data, size_t size, const char *type, Box *out)
{
    BoxIter it = box_children(data, size);
    Box box;
    while (box_next(&it, &box)) {
        if (memcmp(box.type, type, 4) == 0) {
            *out = box;
            return true;
        }
    }
    return false;
}

/* --- Reading moov from disk --------------------------------------------- */

static uint8_t *read_moov(const char *path, size_t *moov_size, char **error)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        *error = format_error("Could not open %s", path);
        return NULL;
    }

    uint8_t *moov = NULL;
    off_t offset = 0;
    uint8_t header[16];
    while (fseeko(f, offset, SEEK_SET) == 0 && fread(header, 1, 8, f) == 8) {
        uint64_t size = read_u32(header);
        size_t header_size = 8;
        if (size == 1) {
            if (fread(header + 8, 1, 8, f) != 8)
                break;
            size = read_u64(header + 8);
            header_size = 16;
        }
        if (size != 0 && size < header_size)
            break;

        if (memcmp(header + 4, "moov", 4) == 0) {
            if (size == 0 || size - header_size > MAX_MOOV_SIZE)
                break;
            *moov_size = (size_t)(size - header_size);
            moov = malloc(*moov_size);
            if (moov && fread(moov, 1, *moov_size, f) != *moov_size) {
                free(moov);
                moov = NULL;
            }
            break;
        }
        if (size == 0)
            break; /* runs to end of file */
        offset += (off_t)size;
    }
    fclose(f);

    if (!moov)
        *error = format_error("Not an MP4 file, or it is damaged");
    return moov;
}

/* --- JSON, just enough for the stem atom -------------------------------- */

typedef struct {
    const char *pos;
    const char *end;
} Json;

static void json_space(Json *j)
{
    while (j->pos < j->end && (*j->pos == ' ' || *j->pos == '\t' || *j->pos == '\n' || *j->pos == '\r'))
        j->pos++;
}

static bool json_eat(Json *j, char c)
{
    json_space(j);
    if (j->pos < j->end && *j->pos == c) {
        j->pos++;
        return true;
    }
    return false;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool json_hex4(Json *j, uint32_t *out)
{
    if (j->end - j->pos < 4)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(j->pos[i]);
        if (d < 0)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    j->pos += 4;
    *out = v;
    return true;
}

static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string. With `out` NULL the string is only skipped. */
static bool json_string(Json *j, char **out)
{
    if (!json_eat(j, '"'))
        return false;
    /* Escapes never grow the text, so the raw length bounds the result. */
    char *s = out ? malloc((size_t)(j->end - j->pos) + 1) : NULL;
    if (out && !s)
        return false;
    size_t n = 0;
    while (j->pos < j->end && *j->pos != '"') {
        char c = *j->pos++;
        if (c != '\\') {
            if (s)
                s[n++] = c;
            continue;
        }
        if (j->pos >= j->end)
            break;
        char e = *j->pos++;
        char decoded = 0;
        switch (e) {
        case 'n': decoded = '\n'; break;
        case 't': decoded = '\t'; break;
        case 'r': decoded = '\r'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case 'u': {
            uint32_t cp;
            if (!json_hex4(j, &cp))
                goto fail;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->pos >= 6 && j->pos[0] == '\\' && j->pos[1] == 'u') {
                uint32_t low;
                j->pos += 2;
                if (!json_hex4(j, &low))
                    goto fail;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            if (s)
                n += utf8_encode(cp, s + n);
            continue;
        }
        default: decoded = e; break; /* \" \\ \/ */
        }
        if (s)
            s[n++] = decoded;
    }
    if (!json_eat(j, '"'))
        goto fail;
    if (out) {
        s[n] = '\0';
        *out = s;
    }
    return true;
fail:
    free(s);
    return false;
}

static bool json_skip_value(Json *j, int depth);

static bool json_skip_container(Json *j, char close, bool keyed, int depth)
{
    if (json_eat(j, close))
        return true;
    do {
        if (keyed && (!json_string(j, NULL) || !json_eat(j, ':')))
            return false;
        if (!json_skip_value(j, depth + 1))
            return false;
    } while (json_eat(j, ','));
    return json_eat(j, close);
}

static bool json_skip_value(Json *j, int depth)
{
    if (depth > 32)
        return false;
    json_space(j);
    if (j->pos >= j->end)
        return false;
    switch (*j->pos) {
    case '"':
        return json_string(j, NULL);
    case '{':
        j->pos++;
        return json_skip_container(j, '}', true, depth);
    case '[':
        j->pos++;
        return json_skip_container(j, ']', false, depth);
    default:
        /* number, true, false or null */
        while (j->pos < j->end && strchr(",}] \t\r\n", *j->pos) == NULL)
            j->pos++;
        return true;
    }
}

static bool parse_color(const char *s, uint32_t *out)
{
    if (s[0] != '#' || strlen(s) < 7)
        return false;
    uint32_t v = 0;
    for (int i = 1; i <= 6; i++) {
        int d = hex_digit(s[i]);
        if (d < 0)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    *out = v;
    return true;
}

/* Parses one {"name": ..., "color": ...} object, ignoring other keys. */
static bool json_stem(Json *j, T10Stem *stem)
{
    if (!json_eat(j, '{'))
        return false;
    if (json_eat(j, '}'))
        return true;
    do {
        char *key = NULL;
        if (!json_string(j, &key) || !json_eat(j, ':')) {
            free(key);
            return false;
        }
        json_space(j);
        bool is_string = j->pos < j->end && *j->pos == '"';
        bool ok;
        if (is_string && strcmp(key, "name") == 0) {
            free(stem->name);
            stem->name = NULL;
            ok = json_string(j, &stem->name);
        } else if (is_string && strcmp(key, "color") == 0) {
            char *color = NULL;
            ok = json_string(j, &color);
            if (ok)
                parse_color(color, &stem->color);
            free(color);
        } else {
            ok = json_skip_value(j, 1);
        }
        free(key);
        if (!ok)
            return false;
    } while (json_eat(j, ','));
    return json_eat(j, '}');
}

/* Fills names and colours from {"stems": [...]}; stops quietly on bad JSON. */
static void parse_stem_json(const uint8_t *data, size_t size, T10StemFile *file)
{
    Json j = {(const char *)data, (const char *)data + size};
    if (!json_eat(&j, '{') || json_eat(&j, '}'))
        return;
    do {
        char *key = NULL;
        if (!json_string(&j, &key) || !json_eat(&j, ':')) {
            free(key);
            return;
        }
        bool is_stems = strcmp(key, "stems") == 0;
        free(key);
        if (!is_stems) {
            if (!json_skip_value(&j, 1))
                return;
            continue;
        }
        if (!json_eat(&j, '['))
            return;
        if (json_eat(&j, ']'))
            return;
        int i = 0;
        do {
            if (i < T10_MAX_STEMS) {
                if (!json_stem(&j, &file->stems[i]))
                    return;
            } else if (!json_skip_value(&j, 1)) {
                return;
            }
            i++;
        } while (json_eat(&j, ','));
        return;
    } while (json_eat(&j, ','));
}

/* --- Tags --------------------------------------------------------------- */

/* Returns the payload of an ilst item's data box, skipping type and locale. */
static bool item_data(const Box *item, const uint8_t **data, size_t *size, uint32_t *type)
{
    Box box;
    if (!box_find(item->data, item->size, "data", &box) || box.size < 8)
        return false;
    *type = read_u32(box.data) & 0xFFFFFF;
    *data = box.data + 8;
    *size = box.size - 8;
    return true;
}

static void parse_ilst(const Box *ilst, T10StemFile *file)
{
    BoxIter it = box_children(ilst->data, ilst->size);
    Box item;
    while (box_next(&it, &item)) {
        const uint8_t *data;
        size_t size;
        uint32_t type;
        if (!item_data(&item, &data, &size, &type))
            continue;

        char **text = NULL;
        if (memcmp(item.type, "\xA9nam", 4) == 0)
            text = &file->title;
        else if (memcmp(item.type, "\xA9" "ART", 4) == 0)
            text = &file->artist;
        else if (memcmp(item.type, "\xA9" "alb", 4) == 0)
            text = &file->album;

        if (text && type == 1 && !*text && size > 0) {
            *text = copy_string(data, size);
        } else if (memcmp(item.type, "covr", 4) == 0 && !file->cover && size > 0) {
            file->cover = malloc(size);
            if (file->cover) {
                memcpy(file->cover, data, size);
                file->cover_size = size;
            }
        }
    }
}

static void parse_meta(const Box *meta, T10StemFile *file)
{
    /* iTunes writes meta as a full box, QuickTime without the 4-byte header. */
    const uint8_t *data = meta->data;
    size_t size = meta->size;
    if (size >= 8 && memcmp(data + 4, "hdlr", 4) != 0) {
        data += 4;
        size -= 4;
    }
    Box ilst;
    if (box_find(data, size, "ilst", &ilst))
        parse_ilst(&ilst, file);
}

static bool is_audio_trak(const Box *trak)
{
    Box mdia, hdlr;
    return box_find(trak->data, trak->size, "mdia", &mdia) && box_find(mdia.data, mdia.size, "hdlr", &hdlr) &&
           hdlr.size >= 12 && memcmp(hdlr.data + 8, "soun", 4) == 0;
}

/* --- Entry points ------------------------------------------------------- */

bool t10_stem_file_read(const char *path, T10StemFile *file, char **error)
{
    memset(file, 0, sizeof *file);
    for (int i = 0; i < T10_MAX_STEMS; i++)
        file->stems[i].color = default_colors[i % 4];

    size_t moov_size;
    uint8_t *moov = read_moov(path, &moov_size, error);
    if (!moov)
        return false;

    BoxIter it = box_children(moov, moov_size);
    Box box;
    while (box_next(&it, &box)) {
        if (memcmp(box.type, "trak", 4) == 0 && is_audio_trak(&box)) {
            file->audio_tracks++;
        } else if (memcmp(box.type, "udta", 4) == 0) {
            Box child;
            if (box_find(box.data, box.size, "stem", &child))
                parse_stem_json(child.data, child.size, file);
            if (box_find(box.data, box.size, "meta", &child))
                parse_meta(&child, file);
        }
    }
    free(moov);

    /* Track 0 is the mixdown, the rest are stems. */
    if (file->audio_tracks < 2) {
        *error = format_error("Not a stem file: it has %d audio track%s", file->audio_tracks,
                              file->audio_tracks == 1 ? "" : "s");
        t10_stem_file_clear(file);
        return false;
    }
    file->stem_count = file->audio_tracks - 1;
    if (file->stem_count > T10_MAX_STEMS)
        file->stem_count = T10_MAX_STEMS;

    for (int i = 0; i < file->stem_count; i++) {
        if (file->stems[i].name)
            continue;
        if (file->stem_count == 4)
            file->stems[i].name = copy_string((const uint8_t *)default_names[i], strlen(default_names[i]));
        else
            file->stems[i].name = format_error("Stem %d", i + 1);
    }
    return true;
}

void t10_stem_file_clear(T10StemFile *file)
{
    for (int i = 0; i < T10_MAX_STEMS; i++)
        free(file->stems[i].name);
    free(file->title);
    free(file->artist);
    free(file->album);
    free(file->cover);
    memset(file, 0, sizeof *file);
}
