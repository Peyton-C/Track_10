/*
 * One pipeline plays every stem so they cannot drift apart:
 *
 *   filesrc ! qtdemux
 *     audio_1 ! queue ! decodebin ! audioconvert ! audioresample ! mixer.sink_0
 *     ...
 *     audio_N ! ...                                               ! mixer.sink_N-1
 *   audiomixer ! scaletempo ! audioconvert ! audioresample ! autoaudiosink
 *
 * Track 0, the mixdown, is left unlinked. Stem volumes are the mixer's
 * per-pad volumes, and speed is a seek with a rate that scaletempo turns
 * into a tempo change without a pitch change.
 *
 * T10_AUDIO_SINK names a different sink element, e.g. fakesink for tests.
 */
#include "track10.h"
#include "stemfile.h"

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long opening waits for the pipeline to preroll. */
#define OPEN_TIMEOUT (10 * GST_SECOND)

struct T10Player {
    T10StemFile file;
    GstElement *pipeline;
    GstElement *mixer;
    GstPad *mixer_pads[T10_MAX_STEMS];
    GstElement *branches[T10_MAX_STEMS];
    GstElement *output_tail; /* the element feeding the sink */
    GstElement *sink;
    GstBus *bus;
    double volumes[T10_MAX_STEMS];
    double rate;
    gint64 duration;
    /* Where a seek is heading until the pipeline settles, or -1. Queries
     * fail or read 0 while a flushing seek prerolls. */
    gint64 seek_target;
    gint64 last_position;
    bool playing;
    char *error;
};

static void on_pad_added(GstElement *demux, GstPad *pad, gpointer data)
{
    (void)demux;
    T10Player *player = data;
    unsigned track;
    const char *name = GST_PAD_NAME(pad);
    if (sscanf(name, "audio_%u", &track) != 1 || track == 0 || track > (unsigned)player->file.stem_count)
        return;
    GstPad *sink = gst_element_get_static_pad(player->branches[track - 1], "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sink);
    if (ret != GST_PAD_LINK_OK)
        g_warning("Track 10: could not link %s: %s", name, gst_pad_link_get_name(ret));
    gst_object_unref(sink);
}

/* decodebin adds its output pad once it has found a decoder. */
static void on_decoded_pad(GstElement *decodebin, GstPad *pad, gpointer data)
{
    (void)decodebin;
    GstElement *convert = data;
    GstPad *sink = gst_element_get_static_pad(convert, "sink");
    if (!gst_pad_is_linked(sink))
        gst_pad_link(pad, sink);
    gst_object_unref(sink);
}

/* queue ! decodebin ! audioconvert ! audioresample, returning the queue. */
static GstElement *add_branch(GstBin *bin, GstPad *mixer_pad)
{
    GstElement *queue = gst_element_factory_make("queue", NULL);
    GstElement *decode = gst_element_factory_make("decodebin", NULL);
    GstElement *convert = gst_element_factory_make("audioconvert", NULL);
    GstElement *resample = gst_element_factory_make("audioresample", NULL);
    if (!queue || !decode || !convert || !resample) {
        GstElement *made[] = {queue, decode, convert, resample};
        for (size_t i = 0; i < G_N_ELEMENTS(made); i++)
            if (made[i])
                gst_object_unref(made[i]);
        return NULL;
    }

    gst_bin_add_many(bin, queue, decode, convert, resample, NULL);
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_decoded_pad), convert);
    GstPad *out = gst_element_get_static_pad(resample, "src");
    bool linked = gst_element_link(queue, decode) && gst_element_link(convert, resample) &&
                  gst_pad_link(out, mixer_pad) == GST_PAD_LINK_OK;
    gst_object_unref(out);
    return linked ? queue : NULL;
}

static char *message_error(GstMessage *msg)
{
    GError *err = NULL;
    gst_message_parse_error(msg, &err, NULL);
    char *message = strdup(err ? err->message : "Unknown playback error");
    g_clear_error(&err);
    return message;
}

static GstElement *make_sink(void)
{
    const char *name = g_getenv("T10_AUDIO_SINK");
    GstElement *sink = gst_element_factory_make(name && *name ? name : "autoaudiosink", NULL);
    if (sink && name && strcmp(name, "fakesink") == 0)
        g_object_set(sink, "sync", TRUE, NULL);
    return sink;
}

static bool build_pipeline(T10Player *player, const char *path, char **error)
{
    player->pipeline = gst_pipeline_new("track10");
    GstElement *src = gst_element_factory_make("filesrc", NULL);
    GstElement *demux = gst_element_factory_make("qtdemux", NULL);
    player->mixer = gst_element_factory_make("audiomixer", NULL);
    GstElement *tempo = gst_element_factory_make("scaletempo", NULL);
    GstElement *convert = gst_element_factory_make("audioconvert", NULL);
    GstElement *resample = gst_element_factory_make("audioresample", NULL);
    GstElement *sink = make_sink();

    GstElement *elements[] = {src, demux, player->mixer, tempo, convert, resample, sink};
    for (size_t i = 0; i < G_N_ELEMENTS(elements); i++) {
        if (!elements[i]) {
            *error = strdup("GStreamer is missing plugins. Install gst-plugins-base, "
                            "gst-plugins-good and gst-libav.");
            for (size_t k = 0; k < G_N_ELEMENTS(elements); k++)
                if (elements[k])
                    gst_object_unref(elements[k]);
            return false;
        }
    }

    g_object_set(src, "location", path, NULL);
    gst_bin_add_many(GST_BIN(player->pipeline), src, demux, player->mixer, tempo, convert, resample, sink, NULL);
    if (!gst_element_link(src, demux) || !gst_element_link_many(player->mixer, tempo, convert, resample, sink, NULL)) {
        *error = strdup("Could not build the playback pipeline");
        return false;
    }

    for (int i = 0; i < player->file.stem_count; i++) {
        player->mixer_pads[i] = gst_element_request_pad_simple(player->mixer, "sink_%u");
        player->branches[i] = add_branch(GST_BIN(player->pipeline), player->mixer_pads[i]);
        if (!player->branches[i]) {
            *error = strdup("Could not connect a stem to the mixer");
            return false;
        }
    }

    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), player);
    player->bus = gst_element_get_bus(player->pipeline);
    player->output_tail = resample;
    player->sink = sink;
    return true;
}

/* Waits for the first frame to reach the sink, so the file is known good. */
static bool preroll(T10Player *player, char **error)
{
    if (gst_element_set_state(player->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        GstMessage *msg = gst_bus_pop_filtered(player->bus, GST_MESSAGE_ERROR);
        *error = msg ? message_error(msg) : strdup("Could not start playback");
        if (msg)
            gst_message_unref(msg);
        return false;
    }

    GstMessage *msg = gst_bus_timed_pop_filtered(player->bus, OPEN_TIMEOUT,
                                                 GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR);
    if (!msg) {
        *error = strdup("Timed out opening the file");
        return false;
    }
    bool ok = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ASYNC_DONE;
    if (!ok)
        *error = message_error(msg);
    gst_message_unref(msg);
    if (!ok)
        return false;

    if (!gst_element_query_duration(player->pipeline, GST_FORMAT_TIME, &player->duration))
        player->duration = 0;
    return true;
}

T10Player *t10_player_open(const char *path, char **error)
{
    char *dummy = NULL;
    if (!error)
        error = &dummy;
    *error = NULL;

    if (!gst_is_initialized())
        gst_init(NULL, NULL);

    T10Player *player = calloc(1, sizeof *player);
    if (!player) {
        *error = strdup("Out of memory");
        return NULL;
    }
    player->rate = 1.0;
    player->seek_target = -1;
    for (int i = 0; i < T10_MAX_STEMS; i++)
        player->volumes[i] = 1.0;

    if (!t10_stem_file_read(path, &player->file, error) || !build_pipeline(player, path, error) ||
        !preroll(player, error)) {
        t10_player_free(player);
        free(dummy);
        return NULL;
    }
    return player;
}

void t10_player_free(T10Player *player)
{
    if (!player)
        return;
    if (player->pipeline) {
        gst_element_set_state(player->pipeline, GST_STATE_NULL);
        for (int i = 0; i < T10_MAX_STEMS; i++) {
            if (player->mixer_pads[i]) {
                gst_element_release_request_pad(player->mixer, player->mixer_pads[i]);
                gst_object_unref(player->mixer_pads[i]);
            }
        }
        gst_object_unref(player->pipeline);
    }
    if (player->bus)
        gst_object_unref(player->bus);
    t10_stem_file_clear(&player->file);
    free(player->error);
    free(player);
}

/* --- Transport ---------------------------------------------------------- */

static void seek_to(T10Player *player, gint64 position)
{
    gst_element_seek(player->pipeline, player->rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                     GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    player->seek_target = position;
    player->last_position = position;
}

unsigned t10_player_poll(T10Player *player)
{
    unsigned events = 0;
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(player->bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE))) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ASYNC_DONE) {
            player->seek_target = -1;
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            t10_player_pause(player);
            seek_to(player, 0);
            events |= T10_EVENT_EOS;
        } else {
            free(player->error);
            player->error = message_error(msg);
            t10_player_pause(player);
            events |= T10_EVENT_ERROR;
        }
        gst_message_unref(msg);
    }
    return events;
}

const char *t10_player_error(const T10Player *player)
{
    return player->error;
}

void t10_player_play(T10Player *player)
{
    if (gst_element_set_state(player->pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE)
        player->playing = true;
}

void t10_player_pause(T10Player *player)
{
    gst_element_set_state(player->pipeline, GST_STATE_PAUSED);
    player->playing = false;
}

bool t10_player_is_playing(const T10Player *player)
{
    return player->playing;
}

double t10_player_position(T10Player *player)
{
    gint64 position;
    if (player->seek_target >= 0)
        position = player->seek_target;
    else if (gst_element_query_position(player->pipeline, GST_FORMAT_TIME, &position) && position >= 0)
        player->last_position = position;
    else
        position = player->last_position;
    return (double)position / GST_SECOND;
}

double t10_player_duration(const T10Player *player)
{
    return (double)player->duration / GST_SECOND;
}

void t10_player_seek(T10Player *player, double seconds)
{
    gint64 position = (gint64)(seconds * GST_SECOND);
    if (position < 0)
        position = 0;
    if (player->duration > 0 && position > player->duration)
        position = player->duration;
    seek_to(player, position);
}

/*
 * A sink binds to the default device when it opens and stays there, so a new
 * device needs a new sink. Swapping one in a running pipeline means handling
 * the lost clock, so instead the whole pipeline stops, gets the new sink,
 * prerolls again and seeks back, which takes a moment but is simple.
 */
bool t10_player_reset_output(T10Player *player)
{
    gint64 position = (gint64)(t10_player_position(player) * GST_SECOND);
    bool was_playing = player->playing;

    gst_element_set_state(player->pipeline, GST_STATE_NULL);
    player->playing = false;
    player->seek_target = -1;

    GstElement *sink = make_sink();
    if (sink) {
        gst_element_unlink(player->output_tail, player->sink);
        gst_bin_remove(GST_BIN(player->pipeline), player->sink);
        gst_bin_add(GST_BIN(player->pipeline), sink);
        gst_element_link(player->output_tail, sink);
        player->sink = sink;
    }

    char *error = NULL;
    if (!preroll(player, &error)) {
        free(player->error);
        player->error = error;
        return false;
    }
    seek_to(player, position);
    if (was_playing)
        t10_player_play(player);
    return true;
}

double t10_player_rate(const T10Player *player)
{
    return player->rate;
}

void t10_player_set_rate(T10Player *player, double rate)
{
    if (rate <= 0 || rate == player->rate)
        return;
    double position = t10_player_position(player);
    player->rate = rate;
    t10_player_seek(player, position);
}

/* --- Stems and tags ----------------------------------------------------- */

static bool valid_stem(const T10Player *player, int stem)
{
    return stem >= 0 && stem < player->file.stem_count;
}

int t10_player_stem_count(const T10Player *player)
{
    return player->file.stem_count;
}

const char *t10_player_stem_name(const T10Player *player, int stem)
{
    return valid_stem(player, stem) ? player->file.stems[stem].name : NULL;
}

uint32_t t10_player_stem_color(const T10Player *player, int stem)
{
    return valid_stem(player, stem) ? player->file.stems[stem].color : 0;
}

double t10_player_stem_volume(const T10Player *player, int stem)
{
    return valid_stem(player, stem) ? player->volumes[stem] : 0;
}

void t10_player_set_stem_volume(T10Player *player, int stem, double volume)
{
    if (!valid_stem(player, stem))
        return;
    volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
    player->volumes[stem] = volume;
    g_object_set(player->mixer_pads[stem], "volume", volume, NULL);
}

const char *t10_player_title(const T10Player *player)
{
    return player->file.title;
}

const char *t10_player_artist(const T10Player *player)
{
    return player->file.artist;
}

const char *t10_player_album(const T10Player *player)
{
    return player->file.album;
}

const uint8_t *t10_player_cover(const T10Player *player, size_t *size)
{
    if (size)
        *size = player->file.cover_size;
    return player->file.cover;
}
