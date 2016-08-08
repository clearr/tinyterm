/*
 * MIT/X Consortium License
 *
 * © 2013 Jakub Klinkovský
 * © 2009 Sebastian Linke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <glib.h>
#include <sys/wait.h>
#include <gdk/gdkkeysyms.h>
#include <vte/vte.h>
#include <signal.h>
#include "config.h"

static int child_pid = 0;   // needs to be global for signal_handler to work
static gint initial_font_size;
static gboolean is_fullscreen = FALSE;

/* spawn xdg-open and pass text as argument */
static void
xdg_open(const char* text)
{
    GError* error = NULL;
    char* command = g_strconcat("xdg-open ", text, NULL);
    g_spawn_command_line_async(command, &error);
    if (error) {
        g_printerr("xdg-open: %s\n", error->message);
        g_error_free(error);
    }
    g_free(command);
}

/* callback to receive data from GtkClipboard */
static void
xdg_open_selection_cb(GtkClipboard* clipboard, const char* string, gpointer data)
{
    xdg_open(string);
}

/* pass selected text to xdg-open */
static void
xdg_open_selection(GtkWidget* terminal)
{
    GdkDisplay* display = gtk_widget_get_display(terminal);;
    GtkClipboard* clipboard = gtk_clipboard_get_for_display(display, GDK_SELECTION_PRIMARY);
    vte_terminal_copy_primary(VTE_TERMINAL (terminal));
    gtk_clipboard_request_text(clipboard, xdg_open_selection_cb, NULL);
}

/* callback to set window urgency hint on beep events */
static void
window_urgency_hint_cb(VteTerminal* vte)
{
    gtk_window_set_urgency_hint(GTK_WINDOW (gtk_widget_get_toplevel(GTK_WIDGET (vte))), TRUE);
}

/* callback to unset window urgency hint on focus */
gboolean
window_focus_cb(GtkWindow* window)
{
    gtk_window_set_urgency_hint(window, FALSE);
    return FALSE;
}

/* callback to dynamically change window title */
static void
window_title_cb(VteTerminal* vte)
{
    gtk_window_set_title(GTK_WINDOW (gtk_widget_get_toplevel(GTK_WIDGET (vte))), vte_terminal_get_window_title(vte));
}

/* apply geometry hints to handle terminal resizing */
static void
set_geometry_hints(VteTerminal* vte)
{
    GtkWidget* vte_widget = GTK_WIDGET(vte);
    GdkGeometry geo_hints;
    glong char_width, char_height;

    char_width = vte_terminal_get_char_width(vte);
    char_height = vte_terminal_get_char_height(vte);
    geo_hints.base_width  = char_width;
    geo_hints.base_height = char_height;
    geo_hints.min_width   = char_width;
    geo_hints.min_height  = char_height;
    geo_hints.width_inc   = char_width;
    geo_hints.height_inc  = char_height;
    gtk_window_set_geometry_hints(GTK_WINDOW(gtk_widget_get_toplevel(vte_widget)), vte_widget, &geo_hints,
                                  GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE | GDK_HINT_BASE_SIZE);
}

/* enlarge/shrink the font */
static void
resize_font(VteTerminal* vte, gint size, gboolean is_absolute)
{
    PangoFontDescription *desc = pango_font_description_copy(vte_terminal_get_font(vte));
    if (!is_absolute)
        size = pango_font_description_get_size(desc) + size * PANGO_SCALE;
    pango_font_description_set_size(desc, size);
    vte_terminal_set_font(vte, desc);
    set_geometry_hints(vte);
    pango_font_description_free(desc);
}

/* toggle fullscreen state */
static void
toggle_fullscreen(VteTerminal* vte)
{
    if (is_fullscreen) {
        is_fullscreen = FALSE;
        gtk_window_unfullscreen(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte))));
    } else {
        is_fullscreen = TRUE;
        gtk_window_fullscreen(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte))));
    }
}

/* callback to react to key press events */
static gboolean
key_press_cb(VteTerminal* vte, GdkEventKey* event)
{
    if ((event->state & (TINYTERM_MODIFIER)) == (TINYTERM_MODIFIER)) {
        switch (gdk_keyval_to_upper(event->keyval)) {
            case TINYTERM_KEY_COPY:
                vte_terminal_copy_clipboard(vte);
                return TRUE;
            case TINYTERM_KEY_PASTE:
                vte_terminal_paste_clipboard(vte);
                return TRUE;
            case TINYTERM_KEY_OPEN:
                xdg_open_selection(vte);
                return TRUE;
            case TINYTERM_KEY_FONT_ENLARGE:
                resize_font(vte, +1, FALSE);
                return TRUE;
            case TINYTERM_KEY_FONT_SHRINK:
                resize_font(vte, -1, FALSE);
                return TRUE;
            case TINYTERM_KEY_FONT_RESET:
                resize_font(vte, initial_font_size, TRUE);
                return TRUE;
        }
    } else if (event->keyval == TINYTERM_KEY_FULLSCREEN) {
        toggle_fullscreen(vte);
        return TRUE;
    }
    return FALSE;
}

static void
vte_config(VteTerminal* vte)
{
    GdkColor color_fg, color_bg;
    GdkColor color_palette[16];
    GRegex* regex = g_regex_new(url_regex, G_REGEX_CASELESS, G_REGEX_MATCH_NOTEMPTY, NULL);
    const PangoFontDescription* desc;

    vte_terminal_search_set_gregex(vte, regex);
    vte_terminal_search_set_wrap_around     (vte, TINYTERM_SEARCH_WRAP_AROUND);
    vte_terminal_set_audible_bell           (vte, TINYTERM_AUDIBLE_BELL);
    vte_terminal_set_visible_bell           (vte, TINYTERM_VISIBLE_BELL);
    vte_terminal_set_cursor_shape           (vte, TINYTERM_CURSOR_SHAPE);
    vte_terminal_set_cursor_blink_mode      (vte, TINYTERM_CURSOR_BLINK);
    vte_terminal_set_word_chars             (vte, TINYTERM_WORD_CHARS);
    vte_terminal_set_scrollback_lines       (vte, TINYTERM_SCROLLBACK_LINES);
    vte_terminal_set_font_from_string_full  (vte, TINYTERM_FONT, TINYTERM_ANTIALIAS);

    desc = vte_terminal_get_font(vte);
    initial_font_size = pango_font_description_get_size(desc);

    /* init colors */
    gdk_color_parse(TINYTERM_COLOR_FOREGROUND, &color_fg);
    gdk_color_parse(TINYTERM_COLOR_BACKGROUND, &color_bg);
    gdk_color_parse(TINYTERM_COLOR0,  &color_palette[0]);
    gdk_color_parse(TINYTERM_COLOR1,  &color_palette[1]);
    gdk_color_parse(TINYTERM_COLOR2,  &color_palette[2]);
    gdk_color_parse(TINYTERM_COLOR3,  &color_palette[3]);
    gdk_color_parse(TINYTERM_COLOR4,  &color_palette[4]);
    gdk_color_parse(TINYTERM_COLOR5,  &color_palette[5]);
    gdk_color_parse(TINYTERM_COLOR6,  &color_palette[6]);
    gdk_color_parse(TINYTERM_COLOR7,  &color_palette[7]);
    gdk_color_parse(TINYTERM_COLOR8,  &color_palette[8]);
    gdk_color_parse(TINYTERM_COLOR9,  &color_palette[9]);
    gdk_color_parse(TINYTERM_COLOR10, &color_palette[10]);
    gdk_color_parse(TINYTERM_COLOR11, &color_palette[11]);
    gdk_color_parse(TINYTERM_COLOR12, &color_palette[12]);
    gdk_color_parse(TINYTERM_COLOR13, &color_palette[13]);
    gdk_color_parse(TINYTERM_COLOR14, &color_palette[14]);
    gdk_color_parse(TINYTERM_COLOR15, &color_palette[15]);

    vte_terminal_set_colors(vte, &color_fg, &color_bg, &color_palette, 16);
}

static void
vte_spawn(VteTerminal* vte, char* working_directory, char* command, char** environment)
{
    GError* error = NULL;
    char** command_argv = NULL;

    /* Parse command into array */
    if (!command)
        command = vte_get_user_shell();
    g_shell_parse_argv(command, NULL, &command_argv, &error);
    if (error) {
        g_printerr("Failed to parse command: %s\n", error->message);
        g_error_free(error);
        exit(EXIT_FAILURE);
    }

    /* Create pty object */
    VtePty* pty = vte_terminal_pty_new(vte, VTE_PTY_NO_HELPER, &error);
    if (error) {
        g_printerr("Failed to create pty: %s\n", error->message);
        g_error_free(error);
        exit(EXIT_FAILURE);
    }
    vte_pty_set_term(pty, TINYTERM_TERMINFO);
    vte_terminal_set_pty_object(vte, pty);

    /* Spawn default shell (or specified command) */
    g_spawn_async(working_directory, command_argv, environment,
                  (G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN),  // flags from GSpawnFlags
                  (GSpawnChildSetupFunc)vte_pty_child_setup, // an extra child setup function to run in the child just before exec()
                  pty,          // user data for child_setup
                  &child_pid,   // a location to store the child PID
                  &error);      // return location for a GError
    if (error) {
        g_printerr("%s\n", error->message);
        g_error_free(error);
        exit(EXIT_FAILURE);
    }
    vte_terminal_watch_child(vte, child_pid);
    g_strfreev(command_argv);
}

/* callback to exit TinyTerm with exit status of child process */
static void
vte_exit_cb(VteTerminal* vte)
{
    int status = vte_terminal_get_child_exit_status(vte);
    gtk_main_quit();
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE);
}

static void
parse_arguments(int argc, char* argv[], char** command, char** directory, gboolean* keep, char** name, char** title)
{
    gboolean version = FALSE;   // show version?
    const GOptionEntry entries[] = {
        {"version",   'v', 0, G_OPTION_ARG_NONE,    &version,   "Display program version and exit.", 0},
        {"execute",   'e', 0, G_OPTION_ARG_STRING,  command,    "Execute command instead of default shell.", "COMMAND"},
        {"directory", 'd', 0, G_OPTION_ARG_STRING,  directory,  "Sets the working directory for the shell (or the command specified via -e).", "PATH"},
        {"keep",      'k', 0, G_OPTION_ARG_NONE,    keep,       "Don't exit the terminal after child process exits.", 0},
        {"name",      'n', 0, G_OPTION_ARG_STRING,  name,       "Set first value of WM_CLASS property; second value is always 'TinyTerm' (default: 'tinyterm')", "NAME"},
        {"title",     't', 0, G_OPTION_ARG_STRING,  title,      "Set value of WM_NAME property; disables window_title_cb (default: 'TinyTerm')", "TITLE"},
        { NULL }
    };

    GError* error = NULL;
    GOptionContext* context = g_option_context_new(NULL);
    g_option_context_set_help_enabled(context, TRUE);
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_parse(context, &argc, &argv, &error);
    g_option_context_free(context);

    if (error) {
        g_printerr("option parsing failed: %s\n", error->message);
        g_error_free(error);
        exit(EXIT_FAILURE);
    }

    if (version) {
        g_print("tinyterm " TINYTERM_VERSION "\n");
        exit(EXIT_SUCCESS);
    }
}

/* UNIX signal handler */
static void
signal_handler(int signal)
{
    if (child_pid != 0)
        kill(child_pid, SIGHUP);
    exit(signal);
}

int
main (int argc, char* argv[])
{
    GtkWidget* window;
    GtkWidget* box;
    GdkPixbuf* icon;
    GtkIconTheme* icon_theme;
    GError* error = NULL;

    /* Variables for parsed command-line arguments */
    char* command = NULL;
    char* directory = NULL;
    gboolean keep = FALSE;
    char* name = NULL;
    char* title = NULL;

    gtk_init(&argc, &argv);
    parse_arguments(argc, argv, &command, &directory, &keep, &name, &title);

    /* Create window */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(window, "delete-event", gtk_main_quit, NULL);
    gtk_window_set_wmclass(GTK_WINDOW (window), name ? name : "tinyterm", "TinyTerm");
    gtk_window_set_title(GTK_WINDOW (window), title ? title : "TinyTerm");

    /* Set window icon supplied by an icon theme */
    icon_theme = gtk_icon_theme_get_default();
    icon = gtk_icon_theme_load_icon(icon_theme, "terminal", 48, 0, &error);
    if (error)
        g_error_free(error);
    if (icon)
        gtk_window_set_icon(GTK_WINDOW (window), icon);

    /* Create main box */
    box = gtk_hbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER (window), box);

    /* Create vte terminal widget */
    GtkWidget* vte_widget = vte_terminal_new();
    gtk_box_pack_start(GTK_BOX (box), vte_widget, TRUE, TRUE, 0);
    VteTerminal* vte = VTE_TERMINAL (vte_widget);
    if (!keep)
        g_signal_connect(vte, "child-exited", G_CALLBACK (vte_exit_cb), NULL);
    g_signal_connect(vte, "key-press-event", G_CALLBACK (key_press_cb), NULL);
    #ifdef TINYTERM_URGENT_ON_BELL
    g_signal_connect(vte, "beep", G_CALLBACK (window_urgency_hint_cb), NULL);
    g_signal_connect(window, "focus-in-event",  G_CALLBACK (window_focus_cb), NULL);
    g_signal_connect(window, "focus-out-event", G_CALLBACK (window_focus_cb), NULL);
    #endif // TINYTERM_URGENT_ON_BELL
    #ifdef TINYTERM_DYNAMIC_WINDOW_TITLE
    if (!title)
        g_signal_connect(vte, "window-title-changed", G_CALLBACK (window_title_cb), NULL);
    #endif // TINYTERM_DYNAMIC_WINDOW_TITLE

    vte_config(vte);
    set_geometry_hints(vte);

    /* Create scrollbar */
    #ifdef TINYTERM_SCROLLBAR_VISIBLE
    GtkWidget* scrollbar;
    scrollbar = gtk_vscrollbar_new(vte->adjustment);
    gtk_box_pack_start(GTK_BOX (box), scrollbar, FALSE, FALSE, 0);
    #endif // TINYTERM_SCROLLBAR_VISIBLE

    vte_spawn(vte, directory, command, NULL);

    /* register signal handler */
    signal(SIGHUP, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* cleanup */
    g_free(command);
    g_free(directory);
    g_free(name);
    g_free(title);

    /* Show widgets and run main loop */
    gtk_widget_show_all(window);
    gtk_main();

    return EXIT_SUCCESS;
}
