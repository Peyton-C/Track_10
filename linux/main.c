/*
 * Track 10's GTK 4 frontend: one window per file, laid out like the macOS
 * app, with the stem mixer under the track info and the transport below.
 */
#include "track10.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>

#define APP_ID "io.github.peyton_c.Track10"

static const double rates[] = {0.5, 0.75, 1, 1.25, 1.5, 2};
static const char *const rate_labels[] = {"0.5×", "0.75×", "1×", "1.25×", "1.5×", "2×", NULL};

typedef struct {
    T10Player *player;
    GtkWindow *window;
    GtkWidget *play_button;
    GtkWidget *position_label;
    GtkWidget *scrubber;
    GtkWidget *stem_names[T10_MAX_STEMS];
    GtkWidget *stem_scales[T10_MAX_STEMS];
    double unmuted[T10_MAX_STEMS];
    guint timer;
    GtkCssProvider *css;
    /* Polling leaves the scrubber alone briefly after the user moves it. */
    gint64 scrubbed_at;
} Player;

static char *format_time(double seconds)
{
    int total = (int)floor(seconds);
    return g_strdup_printf("%02d:%02d", total / 60, total % 60);
}

static void show_error(GtkWindow *parent, const char *heading, const char *detail)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", heading);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
}

/* --- Transport ---------------------------------------------------------- */

static void update_play_button(Player *p)
{
    gtk_button_set_icon_name(GTK_BUTTON(p->play_button), t10_player_is_playing(p->player)
                                                             ? "media-playback-pause-symbolic"
                                                             : "media-playback-start-symbolic");
}

static void update_position(Player *p)
{
    double position = t10_player_position(p->player);
    char *text = format_time(position);
    gtk_label_set_text(GTK_LABEL(p->position_label), text);
    g_free(text);
    if (g_get_monotonic_time() - p->scrubbed_at > 300 * G_TIME_SPAN_MILLISECOND)
        gtk_range_set_value(GTK_RANGE(p->scrubber), position);
}

static gboolean on_tick(gpointer data)
{
    Player *p = data;
    unsigned events = t10_player_poll(p->player);
    if (events & T10_EVENT_ERROR)
        show_error(p->window, "Playback Stopped", t10_player_error(p->player));
    update_play_button(p);
    update_position(p);
    return G_SOURCE_CONTINUE;
}

static void toggle_play(Player *p)
{
    if (t10_player_is_playing(p->player))
        t10_player_pause(p->player);
    else
        t10_player_play(p->player);
    update_play_button(p);
}

static void on_play_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    toggle_play(data);
}

static void skip(Player *p, double seconds)
{
    t10_player_seek(p->player, t10_player_position(p->player) + seconds);
    update_position(p);
}

static void on_back_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    skip(data, -15);
}

static void on_forward_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    skip(data, 15);
}

/* Only fires for user changes, so programmatic updates never seek. */
static gboolean on_scrub(GtkRange *range, GtkScrollType scroll, double value, gpointer data)
{
    (void)range;
    (void)scroll;
    Player *p = data;
    p->scrubbed_at = g_get_monotonic_time();
    t10_player_seek(p->player, value);
    return FALSE;
}

static void on_rate_selected(GObject *dropdown, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    Player *p = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (i < G_N_ELEMENTS(rates))
        t10_player_set_rate(p->player, rates[i]);
}

/* Looks the player up on each press, as dropping a file replaces it. */
static gboolean on_space(GtkWidget *widget, GVariant *args, gpointer data)
{
    (void)args;
    (void)data;
    Player *p = g_object_get_data(G_OBJECT(widget), "player");
    if (p)
        toggle_play(p);
    return TRUE;
}

/* --- Stems -------------------------------------------------------------- */

static int stem_index(GtkWidget *widget)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "stem"));
}

static void update_stem_name(Player *p, int stem)
{
    bool muted = t10_player_stem_volume(p->player, stem) <= 0;
    if (muted)
        gtk_widget_add_css_class(p->stem_names[stem], "muted");
    else
        gtk_widget_remove_css_class(p->stem_names[stem], "muted");
}

static void set_stem_volume(Player *p, int stem, double volume)
{
    t10_player_set_stem_volume(p->player, stem, volume);
    if (volume > 0)
        p->unmuted[stem] = volume;
    gtk_range_set_value(GTK_RANGE(p->stem_scales[stem]), volume);
    update_stem_name(p, stem);
}

static void on_stem_scale(GtkRange *range, gpointer data)
{
    Player *p = data;
    int stem = stem_index(GTK_WIDGET(range));
    double volume = gtk_range_get_value(range);
    if (volume == t10_player_stem_volume(p->player, stem))
        return;
    t10_player_set_stem_volume(p->player, stem, volume);
    if (volume > 0)
        p->unmuted[stem] = volume;
    update_stem_name(p, stem);
}

static void toggle_mute(Player *p, int stem)
{
    bool muted = t10_player_stem_volume(p->player, stem) <= 0;
    set_stem_volume(p, stem, muted ? MAX(p->unmuted[stem], 0.05) : 0);
}

/* Plays only this stem, or everything again when it is already soloed. */
static void solo(Player *p, int stem)
{
    int count = t10_player_stem_count(p->player);
    bool soloed = true;
    for (int i = 0; i < count; i++)
        if ((i == stem) != (t10_player_stem_volume(p->player, i) > 0))
            soloed = false;
    for (int i = 0; i < count; i++) {
        bool on = soloed || i == stem;
        if (on != (t10_player_stem_volume(p->player, i) > 0))
            toggle_mute(p, i);
    }
}

static void on_stem_name_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
    (void)n_press;
    (void)x;
    (void)y;
    Player *p = data;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    if (mods & (GDK_ALT_MASK | GDK_CONTROL_MASK))
        solo(p, stem_index(widget));
    else
        toggle_mute(p, stem_index(widget));
}

/* --- Building the window ------------------------------------------------ */

static char *color_css(uint32_t rgb)
{
    return g_strdup_printf("#%06X", rgb & 0xFFFFFF);
}

/*
 * Stem colours come from the file, so each player gets its own stylesheet.
 * Stylesheets apply to the whole display, so the rules are scoped to a class
 * unique to this player's content, or windows would take each other's colours.
 */
static void add_stem_css(Player *p, GtkWidget *content)
{
    static unsigned next_id;
    char *scope = g_strdup_printf("t10-player-%u", next_id++);
    gtk_widget_add_css_class(content, scope);

    GString *css = g_string_new(NULL);
    int count = t10_player_stem_count(p->player);
    GString *gradient = g_string_new(NULL);
    for (int i = 0; i < count; i++) {
        char *color = color_css(t10_player_stem_color(p->player, i));
        g_string_append_printf(css,
                               ".%s .stem-%d .dot { color: %s; }\n"
                               ".%s .stem-%d.muted .dot { color: alpha(%s, 0.35); }\n"
                               ".%s scale.stem-%d highlight { background: %s; }\n",
                               scope, i, color, scope, i, color, scope, i, color);
        g_string_append_printf(gradient, "%s%s", i ? ", " : "", color);
        g_free(color);
    }
    if (count > 1)
        g_string_append_printf(css, ".%s .cover-placeholder { background-image: linear-gradient(135deg, %s); }\n",
                               scope, gradient->str);

    p->css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p->css, css->str);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p->css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_string_free(css, TRUE);
    g_string_free(gradient, TRUE);
    g_free(scope);
}

static GtkWidget *build_cover(Player *p)
{
    size_t size;
    const uint8_t *bytes = t10_player_cover(p->player, &size);
    if (bytes) {
        GBytes *data = g_bytes_new(bytes, size);
        GdkTexture *texture = gdk_texture_new_from_bytes(data, NULL);
        g_bytes_unref(data);
        if (texture) {
            /* GtkPicture asks for the image's full size, GtkImage for a fixed one. */
            GtkWidget *image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
            g_object_unref(texture);
            gtk_image_set_pixel_size(GTK_IMAGE(image), 200);
            gtk_widget_add_css_class(image, "cover");
            gtk_widget_set_overflow(image, GTK_OVERFLOW_HIDDEN);
            return image;
        }
    }
    GtkWidget *icon = gtk_image_new_from_icon_name("track10-music-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 200, 200);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_widget_add_css_class(box, "cover");
    gtk_widget_add_css_class(box, "cover-placeholder");
    return box;
}

static GtkWidget *info_label(const char *label, const char *value)
{
    char *markup = g_markup_printf_escaped("%s: <b>%s</b>", label, value);
    GtkWidget *widget = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(widget), markup);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(widget, "info");
    g_free(markup);
    return widget;
}

static GtkWidget *build_info(Player *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    const char *title = t10_player_title(p->player);
    GtkWidget *title_label = gtk_label_new(title ? title : "Untitled");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_label_set_wrap(GTK_LABEL(title_label), TRUE);
    gtk_label_set_lines(GTK_LABEL(title_label), 2);
    gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title_label, "title-1");
    gtk_widget_set_margin_bottom(title_label, 4);
    gtk_box_append(GTK_BOX(box), title_label);

    if (t10_player_artist(p->player))
        gtk_box_append(GTK_BOX(box), info_label("Artist", t10_player_artist(p->player)));
    if (t10_player_album(p->player))
        gtk_box_append(GTK_BOX(box), info_label("Album", t10_player_album(p->player)));
    char *duration = format_time(t10_player_duration(p->player));
    gtk_box_append(GTK_BOX(box), info_label("Time", duration));
    g_free(duration);
    return box;
}

static GtkWidget *build_mixer(Player *p)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_add_css_class(grid, "panel");

    for (int i = 0; i < t10_player_stem_count(p->player); i++) {
        char *stem_class = g_strdup_printf("stem-%d", i);

        GtkWidget *name = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *dot = gtk_label_new("●");
        gtk_widget_add_css_class(dot, "dot");
        gtk_box_append(GTK_BOX(name), dot);
        gtk_box_append(GTK_BOX(name), gtk_label_new(t10_player_stem_name(p->player, i)));
        gtk_widget_add_css_class(name, stem_class);
        gtk_widget_add_css_class(name, "stem-name");
        gtk_widget_set_tooltip_text(name, "Click to mute, Ctrl-click to solo");
        gtk_widget_set_cursor_from_name(name, "pointer");
        g_object_set_data(G_OBJECT(name), "stem", GINT_TO_POINTER(i));
        GtkGesture *click = gtk_gesture_click_new();
        g_signal_connect(click, "released", G_CALLBACK(on_stem_name_clicked), p);
        gtk_widget_add_controller(name, GTK_EVENT_CONTROLLER(click));

        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
        gtk_range_set_value(GTK_RANGE(scale), t10_player_stem_volume(p->player, i));
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_widget_add_css_class(scale, stem_class);
        g_object_set_data(G_OBJECT(scale), "stem", GINT_TO_POINTER(i));
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_stem_scale), p);

        p->stem_names[i] = name;
        p->stem_scales[i] = scale;
        p->unmuted[i] = 1;
        gtk_grid_attach(GTK_GRID(grid), name, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), scale, 1, i, 1, 1);
        g_free(stem_class);
    }
    return grid;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip, GCallback callback, Player *p)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", callback, p);
    return button;
}

static GtkWidget *build_transport(Player *p)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "transport");

    gtk_box_append(GTK_BOX(bar), icon_button("track10-skip-back-symbolic", "Back 15 Seconds",
                                             G_CALLBACK(on_back_clicked), p));
    p->play_button = icon_button("media-playback-start-symbolic", "Play/Pause", G_CALLBACK(on_play_clicked), p);
    gtk_box_append(GTK_BOX(bar), p->play_button);
    gtk_box_append(GTK_BOX(bar), icon_button("track10-skip-forward-symbolic", "Forward 15 Seconds",
                                             G_CALLBACK(on_forward_clicked), p));

    p->position_label = gtk_label_new("00:00");
    gtk_widget_add_css_class(p->position_label, "numeric");
    gtk_widget_add_css_class(p->position_label, "dim-label");
    gtk_box_append(GTK_BOX(bar), p->position_label);

    double duration = t10_player_duration(p->player);
    p->scrubber = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, duration > 0 ? duration : 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->scrubber), FALSE);
    gtk_widget_set_hexpand(p->scrubber, TRUE);
    g_signal_connect(p->scrubber, "change-value", G_CALLBACK(on_scrub), p);
    gtk_box_append(GTK_BOX(bar), p->scrubber);

    char *total = format_time(duration);
    GtkWidget *duration_label = gtk_label_new(total);
    g_free(total);
    gtk_widget_add_css_class(duration_label, "numeric");
    gtk_widget_add_css_class(duration_label, "dim-label");
    gtk_box_append(GTK_BOX(bar), duration_label);

    GtkWidget *speed = gtk_drop_down_new_from_strings(rate_labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(speed), 2);
    gtk_widget_set_tooltip_text(speed, "Playback Speed");
    gtk_widget_set_valign(speed, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(speed, "flat");
    g_signal_connect(speed, "notify::selected", G_CALLBACK(on_rate_selected), p);
    gtk_box_append(GTK_BOX(bar), speed);
    return bar;
}

static void player_free(gpointer data)
{
    Player *p = data;
    if (p->timer)
        g_source_remove(p->timer);
    if (p->css) {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p->css));
        g_object_unref(p->css);
    }
    t10_player_free(p->player);
    g_free(p);
}

/* T10_SNAPSHOT=out.png saves the first window to a PNG and quits, so the
 * layout can be checked without a screenshot. */
static gboolean save_snapshot(gpointer data)
{
    GtkWidget *window = data;
    int width = gtk_widget_get_width(window);
    int height = gtk_widget_get_height(window);
    GdkPaintable *paintable = gtk_widget_paintable_new(window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (node) {
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
        GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
        gdk_texture_save_to_png(texture, g_getenv("T10_SNAPSHOT"));
        g_object_unref(texture);
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
    return G_SOURCE_REMOVE;
}

/* Opens a file, reporting failure over `parent`. */
static T10Player *load_player(GtkWindow *parent, GFile *file)
{
    char *path = g_file_get_path(file);
    char *error = NULL;
    T10Player *player = path ? t10_player_open(path, &error) : NULL;
    if (!player) {
        char *name = g_file_get_basename(file);
        char *heading = g_strdup_printf("Can’t Play “%s”", name);
        show_error(parent, heading, error ? error : "The file is not local.");
        g_free(heading);
        g_free(name);
        free(error);
    }
    g_free(path);
    return player;
}

/* Fills `window` with a player for `player`, replacing any there already. */
static void attach_player(GtkWindow *window, T10Player *player, GFile *file)
{
    Player *p = g_new0(Player, 1);
    p->player = player;
    p->window = window;

    char *title = g_file_get_basename(file);
    gtk_window_set_title(window, title);
    g_free(title);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "player");
    add_stem_css(p, content);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_widget_set_size_request(content, 640, -1);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(top, 8);
    gtk_widget_set_margin_end(top, 8);
    GtkWidget *cover = build_cover(p);
    gtk_widget_set_valign(cover, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(top), cover);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_box_append(GTK_BOX(right), build_info(p));
    gtk_box_append(GTK_BOX(right), build_mixer(p));
    gtk_box_append(GTK_BOX(top), right);
    gtk_box_append(GTK_BOX(content), top);
    gtk_box_append(GTK_BOX(content), build_transport(p));

    /* The old content goes first, then the old player it was using. */
    gtk_window_set_child(window, content);
    g_object_set_data_full(G_OBJECT(window), "player", p, player_free);

    p->timer = g_timeout_add(100, on_tick, p);
    t10_player_play(p->player);
    update_play_button(p);
}

/* A file dropped onto a window plays in it, replacing the current track. */
static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    (void)target;
    (void)x;
    (void)y;
    GtkWindow *window = data;
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    if (!files)
        return FALSE;
    GFile *file = files->data;
    g_slist_free(files);

    T10Player *player = load_player(window, file);
    if (!player)
        return FALSE;
    attach_player(window, player, file);
    return TRUE;
}

static void open_window(GtkApplication *app, GFile *file)
{
    T10Player *player = load_player(gtk_application_get_active_window(app), file);
    if (!player)
        return;

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_widget_add_css_class(window, "track10");

    GtkEventController *shortcuts = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcuts), GTK_SHORTCUT_SCOPE_MANAGED);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_space, 0),
                                                          gtk_callback_action_new(on_space, NULL, NULL)));
    gtk_widget_add_controller(window, shortcuts);

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), window);
    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(drop));

    attach_player(GTK_WINDOW(window), player, file);
    gtk_window_present(GTK_WINDOW(window));
    /* Show the focus ring once the keyboard is used, not on the first slider. */
    gtk_window_set_focus_visible(GTK_WINDOW(window), FALSE);
    if (g_getenv("T10_SNAPSHOT"))
        g_timeout_add(1500, save_snapshot, window);
}

/* --- Application -------------------------------------------------------- */

static void on_open_response(GObject *source, GAsyncResult *result, gpointer data)
{
    GtkApplication *app = data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (files) {
        for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
            GFile *file = g_list_model_get_item(files, i);
            open_window(app, file);
            g_object_unref(file);
        }
        g_object_unref(files);
    }
    g_application_release(G_APPLICATION(app));
}

static void on_activate(GApplication *app)
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Stem Files");
    gtk_file_filter_add_suffix(filter, "stem.mp4");
    gtk_file_filter_add_suffix(filter, "mp4");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Stem Files");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    g_application_hold(app);
    gtk_file_dialog_open_multiple(dialog, gtk_application_get_active_window(GTK_APPLICATION(app)), NULL,
                                  on_open_response, app);
    g_object_unref(dialog);
}

static void on_open(GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
    (void)hint;
    (void)data;
    for (int i = 0; i < n_files; i++)
        open_window(GTK_APPLICATION(app), files[i]);
}

static void on_startup(GApplication *app)
{
    (void)app;
    /* Clicking a slider's track jumps there, as on macOS, rather than paging
     * towards the click. Most desktops already default to this; macOS GTK does not. */
    g_object_set(gtk_settings_get_default(), "gtk-primary-button-warps-slider", TRUE, NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
                                      ".track10 .title-1 { font-size: 20pt; font-weight: 800; }\n"
                                      ".track10 .info { font-size: 13pt; opacity: 0.7; }\n"
                                      ".track10 .cover { border-radius: 6px; }\n"
                                      ".track10 .cover-placeholder { color: white; }\n"
                                      ".track10 .panel, .track10 .transport {\n"
                                      "  background: alpha(currentColor, 0.07);\n"
                                      "  padding: 6px 14px; border-radius: 14px; }\n"
                                      ".track10 .transport { border-radius: 999px; padding: 6px 16px; }\n"
                                      ".track10 .stem-name.muted label:not(.dot) { opacity: 0.5; }\n"
                                      ".track10 .numeric { font-feature-settings: 'tnum'; }\n"
                                      ".track10:drop(active) .player {\n"
                                      "  outline: 3px solid alpha(currentColor, 0.5); outline-offset: 8px;\n"
                                      "  border-radius: 12px; }\n"
                                      /* Quick Look's sliders: a thick trough, white knob, neutral fill. */
                                      ".track10 scale trough { min-height: 6px; border-radius: 3px;\n"
                                      "  background: alpha(currentColor, 0.15); border: none; }\n"
                                      ".track10 scale highlight { min-height: 6px; border-radius: 3px;\n"
                                      "  background: alpha(currentColor, 0.9); border: none; margin: 0; }\n"
                                      ".track10 scale slider { background: white; border: none;\n"
                                      "  min-width: 18px; min-height: 18px; margin: -7px;\n"
                                      "  box-shadow: 0 1px 3px alpha(black, 0.4); }\n"
                                      ".track10 .transport button { -gtk-icon-size: 20px; }\n"
                                      ".track10 .transport dropdown > button {\n"
                                      "  background: none; border: none; box-shadow: none; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

int main(int argc, char **argv)
{
    g_set_application_name("Track 10");
    gtk_window_set_default_icon_name(APP_ID);
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
