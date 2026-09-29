/*
 * Prints what Track 10 reads from a stem file. With --test it also plays a
 * few seconds, changing volume, speed and position, to exercise the core
 * without a UI. Set T10_AUDIO_SINK=fakesink to test silently.
 */
#include "track10.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void wait_seconds(T10Player *player, double seconds)
{
    struct timespec step = {0, 50 * 1000 * 1000};
    for (double t = 0; t < seconds; t += 0.05) {
        nanosleep(&step, NULL);
        unsigned events = t10_player_poll(player);
        if (events & T10_EVENT_ERROR) {
            fprintf(stderr, "error: %s\n", t10_player_error(player));
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    bool test = argc == 3 && strcmp(argv[1], "--test") == 0;
    if (argc != 2 && !test) {
        fprintf(stderr, "usage: t10-info [--test] FILE.stem.mp4\n");
        return 2;
    }

    char *error = NULL;
    T10Player *player = t10_player_open(argv[argc - 1], &error);
    if (!player) {
        fprintf(stderr, "error: %s\n", error);
        free(error);
        return 1;
    }

    const char *title = t10_player_title(player);
    const char *artist = t10_player_artist(player);
    const char *album = t10_player_album(player);
    size_t cover_size;
    t10_player_cover(player, &cover_size);
    printf("Title:    %s\n", title ? title : "-");
    printf("Artist:   %s\n", artist ? artist : "-");
    printf("Album:    %s\n", album ? album : "-");
    printf("Duration: %.2f s\n", t10_player_duration(player));
    printf("Cover:    %zu bytes\n", cover_size);
    for (int i = 0; i < t10_player_stem_count(player); i++)
        printf("Stem %d:   %-8s #%06X\n", i + 1, t10_player_stem_name(player, i), t10_player_stem_color(player, i));

    if (test) {
        t10_player_play(player);
        wait_seconds(player, 1);
        printf("after 1s at 1.0x:        %.2f s\n", t10_player_position(player));

        t10_player_set_stem_volume(player, 3, 0);
        t10_player_set_rate(player, 2.0);
        wait_seconds(player, 1);
        printf("after 1s more at 2.0x:   %.2f s\n", t10_player_position(player));

        /* The position must never read as 0 while a seek settles. */
        double before = t10_player_position(player);
        t10_player_seek(player, before + 15);
        double lowest = before + 15;
        struct timespec tick = {0, 20 * 1000 * 1000};
        for (int i = 0; i < 25; i++) {
            /* Read first: the frontends refresh straight after seeking. */
            double now = t10_player_position(player);
            if (now < lowest)
                lowest = now;
            nanosleep(&tick, NULL);
            t10_player_poll(player);
        }
        printf("skip +15s from %.2f s:    lowest reading %.2f s%s\n", before, lowest,
               lowest < before ? "  <-- FAIL" : "");

        double at = t10_player_position(player);
        if (!t10_player_reset_output(player)) {
            fprintf(stderr, "error: %s\n", t10_player_error(player));
            return 1;
        }
        wait_seconds(player, 0.5);
        printf("reset output at %.2f s:   now %.2f s, playing=%d\n", at, t10_player_position(player),
               t10_player_is_playing(player));

        t10_player_set_rate(player, 1.0);
        t10_player_seek(player, t10_player_duration(player) - 0.5);
        wait_seconds(player, 1.5);
        printf("after seeking past end:  %.2f s, playing=%d\n", t10_player_position(player),
               t10_player_is_playing(player));
    }

    t10_player_free(player);
    return 0;
}
