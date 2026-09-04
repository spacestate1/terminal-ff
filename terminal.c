#define _POSIX_C_SOURCE 200809L
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <pty.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "window_manager.h"
#include "font-render.h"
#include "ansi.h"
#include "sound.h"

// Played on Enter. Relative to the exe directory, like the font and UI assets.
#define ENTER_SOUND_PATH "assets/sounds/enter_sound.mp3"

// =============================================================================
// Asset paths
// =============================================================================
//
// Assets are found relative to the executable, never the working directory.
// Two things break a cwd-relative path: launching from anywhere but the
// project folder (a .desktop file, a menu entry, a copy in /usr/local/bin),
// and the built-in `cd`, which calls chdir() and so would strand any texture
// reloaded later on - the 9-slice is re-read every time a split opens.
//
// asset_path() returns a pointer to one of a small ring of static buffers, so
// a caller can pass two of them to the same function (LoadFont takes a JSON
// path and a texture path) without the second overwriting the first.

#define ASSET_RING 4

static char asset_root[PATH_MAX];   // exe directory, with trailing '/'; empty until resolved

// Resolve the directory holding the running executable. Falls back to a
// cwd-relative path if /proc is unavailable, which keeps the old behaviour
// rather than failing outright.
// Does this directory actually hold our assets? Used to reject a candidate
// root before committing to it.
static bool asset_root_is_valid(const char* dir) {
    if (!dir || !dir[0]) return false;

    char probe[PATH_MAX];
    int written = snprintf(probe, sizeof(probe), "%s/assets/fonts/font_basis33.json", dir);
    if (written < 0 || written >= (int)sizeof(probe)) return false;

    return access(probe, R_OK) == 0;
}

static void asset_root_init(const char* argv0) {
    // Candidate roots, best first. /proc/self/exe is normally authoritative,
    // but it names the *loader* when the binary is started through an explicit
    // `ld-linux ... ./terminal` invocation, so each candidate is checked for
    // the assets rather than trusted outright.
    char candidates[2][PATH_MAX];
    int count = 0;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* dir = dirname(exe);   // may modify its argument
        if (dir) snprintf(candidates[count++], PATH_MAX, "%s", dir);
    }

    // argv[0] covers the no-/proc case and the loader case above.
    if (argv0 && strchr(argv0, '/')) {
        char arg_copy[PATH_MAX];
        snprintf(arg_copy, sizeof(arg_copy), "%s", argv0);
        char* dir = dirname(arg_copy);
        if (dir) snprintf(candidates[count++], PATH_MAX, "%s", dir);
    }

    for (int i = 0; i < count; i++) {
        if (!asset_root_is_valid(candidates[i])) continue;
        int written = snprintf(asset_root, sizeof(asset_root), "%s/", candidates[i]);
        if (written > 0 && written < (int)sizeof(asset_root)) return;
    }

    // Nothing verified. Fall back to the first candidate anyway so the error
    // message names a plausible directory, or to cwd-relative if there is
    // none - the font load then fails with a path the user can act on.
    if (count > 0) {
        int written = snprintf(asset_root, sizeof(asset_root), "%s/", candidates[0]);
        if (written > 0 && written < (int)sizeof(asset_root)) return;
    }
    asset_root[0] = '\0';
}

// Absolute path for an asset given relative to the project root.
static const char* asset_path(const char* relative) {
    static char buffers[ASSET_RING][PATH_MAX];
    static int next = 0;

    char* out = buffers[next];
    next = (next + 1) % ASSET_RING;

    if (asset_root[0] == '\0') {
        snprintf(out, PATH_MAX, "%s", relative);
    } else {
        snprintf(out, PATH_MAX, "%s%s", asset_root, relative);
    }
    return out;
}

// LOW LATENCY: Disable verbose debug output for production builds
// Compile with -DDEBUG_VERBOSE to enable detailed logging
#ifdef DEBUG_VERBOSE
    #define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(...) ((void)0)  // No-op
#endif

#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
#define WINDOW_EDGE_PADDING 20  // Padding from window edges
#define TERMINAL_GAP 20         // Gap between split terminals
#define TEXT_MARGIN_LEFT 15
#define TEXT_MARGIN_RIGHT 35    // Extra margin for split mode
#define TEXT_MARGIN_RIGHT_SINGLE 25  // Normal margin for single terminal
#define TEXT_MARGIN_TOP 38
#define TEXT_MARGIN_BOTTOM -55
#define MAX_TERMINAL_LINES 1000
#define MAX_LINE_LENGTH 256

// Terminal state
typedef struct {
    char lines[MAX_TERMINAL_LINES][MAX_LINE_LENGTH];  // Display lines (after wrapping)
    char raw_lines[MAX_TERMINAL_LINES][MAX_LINE_LENGTH];  // Original unwrapped lines
    int line_count;  // Number of display lines
    int raw_line_count;  // Number of raw lines
    int scroll_offset;
    char input_buffer[MAX_LINE_LENGTH];
    int cursor_pos;
    bool cursor_visible;
    float cursor_blink_timer;
    char prompt[MAX_LINE_LENGTH];  // Bash-style prompt

    // Command history
    char history[MAX_TERMINAL_LINES][MAX_LINE_LENGTH];
    int history_count;
    int history_index;  // Current position in history (-1 = not browsing)
    char history_temp[MAX_LINE_LENGTH];  // Temporary storage for current input
} TerminalState;

TerminalState terminal = {0};
AnsiTerminal ansi_term;
WindowManager wm;
Window* term_window;

// PTY state for interactive programs
int pty_master_fd = -1;
pid_t pty_child_pid = -1;
bool interactive_mode = false;
double pty_start_time = 0.0;  // Timestamp when PTY was started
int ansi_scroll_offset = 0;  // Manual scroll offset for ANSI buffer (0 = follow cursor)

// Tab completion state
double last_tab_time = 0.0;
char last_tab_completion[MAX_LINE_LENGTH] = "";
// Holds "<dir>/<match>", so it needs room for two MAX_LINE_LENGTH components.
char last_completed_path[MAX_LINE_LENGTH * 2] = "";

// Split terminal state
bool split_horizontal = false;  // Left/right split
bool split_vertical = false;     // Top/bottom split
int focused_terminal = 0;  // 0 = left/top, 1 = right/bottom
Window* term_window_right = NULL;   // Right terminal (horizontal split)
Window* term_window_bottom = NULL;  // Bottom terminal (vertical split)

// Right terminal state (for independent split terminal - horizontal split)
TerminalState terminal_right = {0};
AnsiTerminal ansi_term_right;
int pty_master_fd_right = -1;
pid_t pty_child_pid_right = -1;
bool interactive_mode_right = false;
double pty_start_time_right = 0.0;
int ansi_scroll_offset_right = 0;  // Manual scroll offset for right ANSI buffer

// Bottom terminal state (for independent split terminal - vertical split)
TerminalState terminal_bottom = {0};
AnsiTerminal ansi_term_bottom;
int pty_master_fd_bottom = -1;
pid_t pty_child_pid_bottom = -1;
bool interactive_mode_bottom = false;
double pty_start_time_bottom = 0.0;
int ansi_scroll_offset_bottom = 0;  // Manual scroll offset for bottom ANSI buffer

// Context menu
typedef struct {
    bool visible;
    float x, y;
    int selected_item;
    double mouse_x, mouse_y;
    float text_y_offset;  // Adjustable Y offset for text positioning
} ContextMenu;

ContextMenu context_menu = {false, 0, 0, -1, 0, 0, 22.0f};  // Default offset = 22

// OPTIMIZATION: Cache line width calculation to avoid repeated computation
int cached_line_width = -1;  // -1 = not yet calculated

#define MENU_ITEM_HEIGHT 30.0f
#define MENU_WIDTH 280.0f

const char* menu_items[] = {
    "Copy",
    "Paste",
    "Clear Screen",
    "Font Size +",
    "Font Size -",
    "Split Vertical",
    "Split Horizontal",
    "Close Split",
    "Sound: On",     // label is replaced at draw time by menu_item_label()
    "Exit"
};
#define MENU_ITEM_COUNT 10
#define MENU_ITEM_SOUND 8

// Label for a menu row. Everything is static except the sound entry, which
// shows what a click will do next.
static const char* menu_item_label(int index) {
    if (index == MENU_ITEM_SOUND) {
        if (!sound_is_available()) return "Sound: unavailable";
        return sound_is_enabled() ? "Sound: On" : "Sound: Off";
    }
    return menu_items[index];
}

// Function declarations
GLuint loadTexture(const char* path);
void terminal_add_line(const char* text);
void terminal_execute_command(const char* cmd);
void terminal_init();
void terminal_update_prompt();
void start_interactive_shell(const char* cmd);
void poll_pty();
void send_key_to_pty(int key);
void resize_pty_to_window();
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void char_callback(GLFWwindow* window, unsigned int codepoint);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);

// Convert Unicode codepoint to UTF-8 bytes
// Returns number of bytes written (1-4), or 0 if codepoint is invalid
// NOTE: Current font rendering only supports ASCII (32-126), so non-ASCII
// characters will be sent to PTY as UTF-8 but may display as '?' or box chars
static int codepoint_to_utf8(unsigned int codepoint, char* out) {
    if (codepoint < 0x80) {
        // 1-byte ASCII
        out[0] = (char)codepoint;
        return 1;
    } else if (codepoint < 0x800) {
        // 2-byte sequence
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint < 0x10000) {
        // 3-byte sequence
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else if (codepoint < 0x110000) {
        // 4-byte sequence
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0; // Invalid codepoint
}

// Load texture function (for 9-slice borders)
GLuint loadTexture(const char* path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(0);  // Don't flip - 9-slice coordinates expect non-flipped

    // CRITICAL: Force 4 channels to prevent buffer overrun with 1/2 channel images
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 4);

    if (!data) {
        printf("Failed to load texture: %s\n", path);
        return 0;
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Data is always RGBA now due to forced conversion
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);
    printf("Loaded texture: %s (%dx%d, %d channels -> RGBA)\n", path, width, height, nrChannels);

    return texture;
}

// Load command history from ~/.bash_history
void terminal_load_history() {
    terminal.history_count = 0;
    terminal.history_index = -1;

    const char* home = getenv("HOME");
    if (!home) {
        printf("WARNING: HOME environment variable not set, history disabled\n");
        return;
    }

    char history_path[512];
    snprintf(history_path, sizeof(history_path), "%s/.bash_history", home);

    printf("Loading command history from: %s\n", history_path);

    FILE* file = fopen(history_path, "r");
    if (!file) {
        printf("Could not open bash history file: %s\n", history_path);
        perror("Error");
        return;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file) && terminal.history_count < MAX_TERMINAL_LINES) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;

        // Skip empty lines
        if (strlen(line) == 0) continue;

        // Parse bash history format: "  123  2025-11-06 15:06:39 command here"
        // Skip leading spaces and line number
        const char* cmd = line;

        // Skip leading whitespace
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        // Skip line number if present
        if (*cmd >= '0' && *cmd <= '9') {
            while (*cmd >= '0' && *cmd <= '9') cmd++;
            while (*cmd == ' ' || *cmd == '\t') cmd++;
        }

        // Skip timestamp if present (format: YYYY-MM-DD HH:MM:SS)
        // Look for date pattern: digits-digits-digits
        if (*cmd >= '0' && *cmd <= '9') {
            const char* check = cmd;
            // Skip date: YYYY-MM-DD
            while (*check && ((*check >= '0' && *check <= '9') || *check == '-')) check++;
            // Skip space
            if (*check == ' ') check++;
            // Skip time: HH:MM:SS
            while (*check && ((*check >= '0' && *check <= '9') || *check == ':')) check++;
            // If we found a valid timestamp pattern, skip it
            if (*check == ' ' && check > cmd + 10) {
                cmd = check + 1;
            }
        }

        // Skip any remaining whitespace
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        // Skip if no actual command remains
        if (strlen(cmd) == 0) continue;

        // Store in history
        strncpy(terminal.history[terminal.history_count], cmd, MAX_LINE_LENGTH - 1);
        terminal.history[terminal.history_count][MAX_LINE_LENGTH - 1] = '\0';
        terminal.history_count++;
    }

    fclose(file);
    printf("Loaded %d commands from bash history\n", terminal.history_count);
    if (terminal.history_count > 0) {
        printf("Most recent command: %s\n", terminal.history[terminal.history_count - 1]);
    }
}

// Update bash-style prompt
void terminal_update_prompt() {
    char username[64] = "user";
    char hostname[64] = "localhost";
    char cwd[256] = "~";
    char short_cwd[64];

    // Get username
    const char* user = getenv("USER");
    if (user) {
        strncpy(username, user, sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
    }

    // Get hostname
    gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';

    // Get current working directory
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // Get basename of directory for shorter display
        char* base = basename(cwd);
        if (base) {
            strncpy(short_cwd, base, sizeof(short_cwd) - 1);
            short_cwd[sizeof(short_cwd) - 1] = '\0';
        } else {
            strncpy(short_cwd, cwd, sizeof(short_cwd) - 1);
        }
    } else {
        strncpy(short_cwd, "~", sizeof(short_cwd) - 1);
    }

    // Format prompt: [user@hostname dir]$
    snprintf(terminal.prompt, sizeof(terminal.prompt), "[%s@%s %s]$ ",
             username, hostname, short_cwd);
}

// Start interactive shell in PTY
// Columns that fit in a pane, and therefore the width advertised to the child
// via TIOCSWINSZ. This is the single source of truth: the PTY, the ANSI
// terminal's autowrap column and the renderer all have to agree on it. When
// they did not, long lines wrapped in the child but not in the emulator and
// ended up overwriting the start of their own row.
static int terminal_cols_for_window(Window* win) {
    if (!win || !win->font) return 80;

    float char_w = GetAverageCharWidth(win->font, win->font_size.value);
    if (char_w <= 0.0f) return 80;

    float usable_w = win->size.width
                   - win->text_margins.left
                   - win->text_margins.right
                   - win->border_size.size * 2.0f;

    int cols = (int)(usable_w / char_w);

    // Safety margin so glyphs never bleed past the 9-slice border.
    if (cols > 6) cols -= 6;
    if (cols < 20) cols = 20;
    if (cols > ANSI_BUFFER_COLS) cols = ANSI_BUFFER_COLS;
    return cols;
}

// Replace the calling (child) process with an interactive shell. bash is the
// default environment; if it is not installed we fall over to sh so the
// terminal still comes up on a minimal system. Only returns if both fail.
static void exec_default_shell(void) {
    // Honour the user's chosen shell before assuming bash. $SHELL is what
    // chsh sets, so a zsh or fish user gets their own shell here rather than
    // being silently dropped into bash.
    const char* user_shell = getenv("SHELL");

    // -l as well as -i. An interactive non-login bash reads only ~/.bashrc,
    // so anything exported from ~/.bash_profile or ~/.profile - commonly PATH
    // entries for version managers like tfenv, nvm, pyenv, and ~/.local/bin -
    // was missing, and those tools appeared not to be installed. A login
    // shell reads the profile files, which is what every other terminal
    // emulator gives you.
    //
    // BASH_COMPLETION_USER_FILE is deliberately not set: it names the *user*
    // completion file (default ~/.bash_completion), so pointing it at the
    // system loader both misused the variable and suppressed the user's own.
    // The system completions load from /etc/bash.bashrc on their own.
    if (user_shell && user_shell[0] == '/') {
        // argv[0] with a leading '-' is the convention that marks a login
        // shell, and it is what zsh and fish read; bash also accepts -l.
        const char* base = strrchr(user_shell, '/');
        base = base ? base + 1 : user_shell;

        char argv0[64];
        snprintf(argv0, sizeof(argv0), "-%s", base);

        execl(user_shell, argv0, "-i", (char*)NULL);
        // Falls through to bash if $SHELL is set but unusable.
    }

    execlp("bash", "-bash", "-l", "-i", (char*)NULL);

    // stderr is the PTY slave here, so this lands in the terminal window and
    // explains why the prompt that follows is sh rather than bash.
    fprintf(stderr, "bash unavailable (%s), falling back to sh\r\n", strerror(errno));

    execlp("sh", "-sh", "-i", (char*)NULL);
    fprintf(stderr, "sh unavailable (%s): no shell to run\r\n", strerror(errno));
}

// A pane narrower or shorter than this is not a terminal any more, it is a
// sliver. These are the floors the window minimum is built from.
#define MIN_PANE_COLS 24
#define MIN_PANE_ROWS 10

// Smallest pane width terminal_cols_for_window() still reports MIN_PANE_COLS
// for. Measured against the real function instead of re-deriving the formula,
// so it cannot drift from the layout the renderer actually uses.
static uint32_t pane_width_for_cols(Window* win, int min_cols) {
    if (!win) return 640;

    WindowSize saved = win->size;
    uint32_t result = 1024;   // fallback if nothing in range qualifies

    for (uint32_t w = 64; w < 4096; w += 8) {
        win->size = WINDOW_SIZE(w, saved.height);
        if (terminal_cols_for_window(win) >= min_cols) {
            result = w;
            break;
        }
    }

    win->size = saved;
    return result;
}

// Height a pane needs for min_rows text rows. The vertical chrome is all
// positive here, so unlike the width this one inverts cleanly.
static uint32_t pane_height_for_rows(Window* win, int min_rows) {
    if (!win) return 400;

    float char_h = win->font_size.value * 16.0f * (1.0f + win->line_spacing.value);
    if (char_h < 1.0f) char_h = 1.0f;

    float chrome = (float)win->text_margins.top + (float)win->text_margins.bottom
                 + win->border_size.size * 2.0f;

    // The renderer reserves 0.75 of a line above the first row and drops one
    // row so descenders are not clipped.
    return (uint32_t)((min_rows + 1.75f) * char_h + chrome) + 1;
}

// Constrain the window so no pane can be squeezed into uselessness. The
// minimum depends on the split mode - side by side needs twice the width,
// stacked needs twice the height - and on the font size, so it is reapplied
// whenever either changes.
static void apply_window_size_limits(GLFWwindow* window) {
    if (!window || !term_window || !term_window->font) return;

    uint32_t pane_w = pane_width_for_cols(term_window, MIN_PANE_COLS);
    uint32_t pane_h = pane_height_for_rows(term_window, MIN_PANE_ROWS);

    int min_w = (int)pane_w + WINDOW_EDGE_PADDING * 2;
    int min_h = (int)pane_h + WINDOW_EDGE_PADDING * 2;

    if (split_horizontal) {
        min_w = (int)(pane_w * 2) + TERMINAL_GAP + WINDOW_EDGE_PADDING * 2;
    } else if (split_vertical) {
        min_h = (int)(pane_h * 2) + TERMINAL_GAP + WINDOW_EDGE_PADDING * 2;
    }

    glfwSetWindowSizeLimits(window, min_w, min_h, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // Raising the limit does not resize a window that is already smaller, so
    // grow it here. Without this, splitting inside a small window produces
    // exactly the collapsed pane the limit exists to prevent.
    int cur_w = 0, cur_h = 0;
    glfwGetWindowSize(window, &cur_w, &cur_h);
    if (cur_w < min_w || cur_h < min_h) {
        glfwSetWindowSize(window,
                          cur_w < min_w ? min_w : cur_w,
                          cur_h < min_h ? min_h : cur_h);
    }
}

void start_interactive_shell(const char* cmd) {
    if (pty_master_fd != -1) return; // already running

    // Calculate actual window size to avoid initial resize shock
    int rows = 24, cols = 80;  // Safe defaults
    if (term_window && term_window->font) {
        float char_h = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);
        float border_padding = term_window->border_size.size * 2.0f;
        float usable_h = term_window->size.height - term_window->text_margins.top -
                        term_window->text_margins.bottom - border_padding;

        // Account for vertical padding for first line (matches rendering)
        float vertical_padding = char_h * 0.75f;
        usable_h -= vertical_padding;

        cols = terminal_cols_for_window(term_window);
        rows = (int)(usable_h / char_h);

        if (rows > 3) rows -= 3;  // Match PTY resize: 2 for safety + 1 for descender clipping
        if (rows < 10) rows = 10;
        if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;
    }

    // The ANSI terminal must autowrap at exactly the width the child is given.
    ansi_set_size(&ansi_term, cols, rows);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    pty_child_pid = forkpty(&pty_master_fd, NULL, NULL, &ws);
    if (pty_child_pid < 0) {
        perror("forkpty");
        pty_master_fd = -1;
        return;
    }

    if (pty_child_pid == 0) {
        // Child: becomes the shell / program

        // forkpty already created a new session and controlling terminal
        // DO NOT call setsid() - it would detach from the controlling terminal

        // The PTY slave is already stdin/stdout/stderr thanks to forkpty
        // Close any other inherited file descriptors
        for (int fd = 3; fd < 256; fd++) {
            close(fd);
        }

        // Set terminal type - use xterm-256color for full feature support
        setenv("TERM", "xterm-256color", 1);

        // Suppress terminal capability warnings during bash startup
        setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);

        // Readline finds /etc/inputrc and ~/.inputrc on its own. Only point
        // INPUTRC at the system file if it actually exists: Arch and minimal
        // Fedora images ship without one, and naming a missing file makes
        // readline skip the user's ~/.inputrc as well, silently dropping
        // their key bindings.
        if (access("/etc/inputrc", R_OK) == 0) {
            setenv("INPUTRC", "/etc/inputrc", 0);
        }

        // Configure terminal attributes for proper interactive shell operation
        struct termios tios;
        if (tcgetattr(STDIN_FILENO, &tios) == 0) {
            // Disable canonical mode so bash can use its own line editing (readline)
            tios.c_lflag &= ~ICANON;
            // Enable echo, backspace erase, and signal generation
            tios.c_lflag |= ECHO | ECHOE | ECHOK | ISIG;
            // Enable output processing (OPOST) and newline conversion (ONLCR)
            tios.c_oflag |= OPOST | ONLCR;
            // Set input flags: CR to NL conversion, enable flow control
            tios.c_iflag |= ICRNL | IXON;
            // Set erase character to backspace/DEL (0x7f)
            tios.c_cc[VERASE] = 0x7f;
            // Set minimum chars and timeout for non-canonical mode
            tios.c_cc[VMIN] = 1;
            tios.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &tios);
        }

        if (cmd && *cmd) {
            execlp("sh", "sh", "-c", cmd, (char*)NULL);
            perror("execlp sh -c");
        } else {
            exec_default_shell();
        }

        _exit(1);
    }

    // Parent: non-blocking master
    int flags = fcntl(pty_master_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pty_master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    ansi_clear(&ansi_term);
    interactive_mode = true;
    pty_start_time = glfwGetTime();  // Record PTY start time for filtering

    // Immediately resize PTY to actual window dimensions
    // This ensures programs like 'top' get the correct size from the start
    resize_pty_to_window();
}

// Poll PTY for output
void poll_pty() {
    if (pty_master_fd < 0) return;

    char buf[4096];
    int read_count = 0;
    const int max_reads = 10; // Reduced to prevent UI freeze - process max 40KB per frame
    bool child_exited = false;

    for (;;) {
        if (read_count++ > max_reads) {
            // Safety: prevent getting stuck in read loop
            // More data will be read in the next frame
            break;
        }

        ssize_t n = read(pty_master_fd, buf, sizeof(buf) - 1); // Leave room for null terminator
        if (n > 0) {
            // Ensure buffer is safe for processing
            buf[n] = '\0';

            // Only filter startup error messages during the first 2 seconds
            bool should_filter = false;
            double elapsed = glfwGetTime() - pty_start_time;
            if (elapsed < 2.0) {
                // Filter out bash startup warning messages
                if (strstr(buf, "Cannot get terminal settings") != NULL ||
                    strstr(buf, "SLang_getkey") != NULL ||
                    strstr(buf, "Assuming EOF on stdin") != NULL ||
                    strstr(buf, "Failed to open terminal") != NULL) {
                    should_filter = true;
                }
            }

            if (!should_filter) {
                ansi_process_output(&ansi_term, buf, (int)n);
            }
            // Continue reading to drain any remaining buffered data
        } else if (n == -1 && errno == EINTR) {
            // A signal landed mid-read. The shutdown handler deliberately does
            // not use SA_RESTART - restarting this read is what stopped the
            // loop from ever noticing the flag - so treat it as "nothing right
            // now" and come back on the next poll rather than as an error,
            // which would tear the session down.
            break;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // no more data right now - safe to exit if child has exited
            if (child_exited) {
                close(pty_master_fd);
                pty_master_fd = -1;
                interactive_mode = false;
                terminal_add_line("[process exited]");
            }
            break;
        } else if (n == 0) {
            // child exited - mark it but keep reading buffered data
            if (pty_child_pid > 0) {
                int status;
                waitpid(pty_child_pid, &status, WNOHANG);
                pty_child_pid = -1;
            }
            child_exited = true;
            // Don't close yet - there may be buffered output
            // Continue reading until EAGAIN
        } else {
            // real error (EIO is common when child exits)
            if (errno != EIO) {
                perror("read pty");
            }
            // Reap the zombie process
            if (pty_child_pid > 0) {
                int status;
                waitpid(pty_child_pid, &status, WNOHANG);
                pty_child_pid = -1;
            }
            close(pty_master_fd);
            pty_master_fd = -1;
            interactive_mode = false;
            terminal_add_line("[process terminated]");
            break;
        }
    }
}

// Poll PTY for right terminal
void poll_pty_right() {
    if (pty_master_fd_right < 0) return;

    char buf[4096];
    int read_count = 0;
    const int max_reads = 10; // Reduced to prevent UI freeze - process max 40KB per frame
    bool child_exited = false;

    for (;;) {
        if (read_count++ > max_reads) {
            break;
        }

        ssize_t n = read(pty_master_fd_right, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';

            // Filter startup messages for first 2 seconds
            bool should_filter = false;
            double elapsed = glfwGetTime() - pty_start_time_right;
            if (elapsed < 2.0) {
                if (strstr(buf, "Cannot get terminal settings") != NULL ||
                    strstr(buf, "SLang_getkey") != NULL ||
                    strstr(buf, "Assuming EOF on stdin") != NULL ||
                    strstr(buf, "Failed to open terminal") != NULL) {
                    should_filter = true;
                }
            }

            if (!should_filter) {
                ansi_process_output(&ansi_term_right, buf, (int)n);
            }
            // Continue reading to drain buffered data
        } else if (n == -1 && errno == EINTR) {
            // A signal landed mid-read. The shutdown handler deliberately does
            // not use SA_RESTART - restarting this read is what stopped the
            // loop from ever noticing the flag - so treat it as "nothing right
            // now" and come back on the next poll rather than as an error,
            // which would tear the session down.
            break;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // no more data - safe to exit if child has exited
            if (child_exited) {
                close(pty_master_fd_right);
                pty_master_fd_right = -1;
                interactive_mode_right = false;
            }
            break;
        } else if (n == 0) {
            // child exited - mark it but keep reading buffered data
            if (pty_child_pid_right > 0) {
                int status;
                waitpid(pty_child_pid_right, &status, WNOHANG);
                pty_child_pid_right = -1;
            }
            child_exited = true;
            // Don't close yet - continue reading until EAGAIN
        } else {
            if (errno != EIO) {
                perror("read pty_right");
            }
            if (pty_child_pid_right > 0) {
                int status;
                waitpid(pty_child_pid_right, &status, WNOHANG);
                pty_child_pid_right = -1;
            }
            close(pty_master_fd_right);
            pty_master_fd_right = -1;
            interactive_mode_right = false;
            break;
        }
    }
}

// Poll PTY for bottom terminal
void poll_pty_bottom() {
    if (pty_master_fd_bottom < 0) return;

    char buf[4096];
    int read_count = 0;
    const int max_reads = 10; // Reduced to prevent UI freeze - process max 40KB per frame
    bool child_exited = false;

    for (;;) {
        if (read_count++ > max_reads) break;

        ssize_t n = read(pty_master_fd_bottom, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            bool should_filter = false;
            double elapsed = glfwGetTime() - pty_start_time_bottom;
            if (elapsed < 2.0) {
                if (strstr(buf, "Cannot get terminal settings") != NULL ||
                    strstr(buf, "SLang_getkey") != NULL ||
                    strstr(buf, "Assuming EOF on stdin") != NULL ||
                    strstr(buf, "Failed to open terminal") != NULL) {
                    should_filter = true;
                }
            }
            if (!should_filter) {
                ansi_process_output(&ansi_term_bottom, buf, (int)n);
            }
            // Continue reading to drain buffered data
        } else if (n == -1 && errno == EINTR) {
            // A signal landed mid-read. The shutdown handler deliberately does
            // not use SA_RESTART - restarting this read is what stopped the
            // loop from ever noticing the flag - so treat it as "nothing right
            // now" and come back on the next poll rather than as an error,
            // which would tear the session down.
            break;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // no more data - safe to exit if child has exited
            if (child_exited) {
                close(pty_master_fd_bottom);
                pty_master_fd_bottom = -1;
                interactive_mode_bottom = false;
            }
            break;
        } else if (n == 0) {
            // child exited - mark it but keep reading buffered data
            if (pty_child_pid_bottom > 0) {
                int status;
                waitpid(pty_child_pid_bottom, &status, WNOHANG);
                pty_child_pid_bottom = -1;
            }
            child_exited = true;
            // Don't close yet - continue reading until EAGAIN
        } else {
            if (errno != EIO) perror("read pty_bottom");
            if (pty_child_pid_bottom > 0) {
                int status;
                waitpid(pty_child_pid_bottom, &status, WNOHANG);
                pty_child_pid_bottom = -1;
            }
            close(pty_master_fd_bottom);
            pty_master_fd_bottom = -1;
            interactive_mode_bottom = false;
            break;
        }
    }
}

// Send key to PTY
void send_key_to_pty(int key) {
    if (pty_master_fd < 0) return;

    const char *seq = NULL;
    char c;
    ssize_t result;

    switch (key) {
        case GLFW_KEY_ESCAPE:
            c = 0x1b;  // ESC character
            result = write(pty_master_fd, &c, 1);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("write to pty");
            }
            return;
        case GLFW_KEY_ENTER:
            c = '\r';
            result = write(pty_master_fd, &c, 1);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("write to pty");
            }
            return;
        case GLFW_KEY_BACKSPACE:
            c = 0x7f;
            result = write(pty_master_fd, &c, 1);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("write to pty");
            }
            return;
        case GLFW_KEY_TAB:
            c = '\t';
            result = write(pty_master_fd, &c, 1);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("write to pty");
            }
            return;

        // Arrow keys
        case GLFW_KEY_UP:    seq = "\x1b[A"; break;
        case GLFW_KEY_DOWN:  seq = "\x1b[B"; break;
        case GLFW_KEY_RIGHT: seq = "\x1b[C"; break;
        case GLFW_KEY_LEFT:  seq = "\x1b[D"; break;

        // Home/End/Page/Delete keys
        case GLFW_KEY_HOME:      seq = "\x1b[H"; break;
        case GLFW_KEY_END:       seq = "\x1b[F"; break;
        case GLFW_KEY_PAGE_UP:   seq = "\x1b[5~"; break;
        case GLFW_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
        case GLFW_KEY_DELETE:    seq = "\x1b[3~"; break;
    }

    if (seq) {
        result = write(pty_master_fd, seq, strlen(seq));
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("write to pty");
        }
    }
}

// Resize PTY to match window
void resize_pty_to_window() {
    if (pty_master_fd < 0 || !term_window || !term_window->font) return;

    // Get actual character dimensions from font metrics
    float char_h = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);

    // Text content area inside window (respect margins + border padding)
    float border_padding = term_window->border_size.size * 2.0f;
    float usable_h = term_window->size.height
                   - term_window->text_margins.top
                   - term_window->text_margins.bottom
                   - border_padding;

    // Account for vertical padding for first line (0.75 * line_height)
    float vertical_padding = char_h * 0.75f;
    usable_h -= vertical_padding;

    int cols = terminal_cols_for_window(term_window);
    int rows = (int)(usable_h / char_h);

    // Subtract 3 rows total: 2 for safety + 1 to match rendering descender clipping
    if (rows > 3) rows -= 3;

    if (rows < 10) rows = 10;  // Ensure minimum rows for programs like top

    // Don't advertise more than we can store in ansi_term
    if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;

    // Keep the emulator's autowrap column in step with what the child is told.
    ansi_set_size(&ansi_term, cols, rows);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    DEBUG_PRINT("Left PTY size: %d rows x %d cols (usable height: %.0f, char height: %.1f)\n",
                rows, cols, usable_h, char_h);

    // Check if pty_master_fd is a valid terminal before ioctl
    if (isatty(pty_master_fd)) {
        if (ioctl(pty_master_fd, TIOCSWINSZ, &ws) < 0) {
            // Silently handle ioctl failures - PTY may not support it yet
            // or may be in an intermediate state
        }
        if (pty_child_pid > 0) {
            kill(pty_child_pid, SIGWINCH);
        }
    }
}

// Resize right PTY to match window
void resize_pty_to_window_right() {
    if (pty_master_fd_right < 0 || !term_window_right || !term_window_right->font) return;

    // Get actual character dimensions from font metrics
    float char_h = term_window_right->font_size.value * 16.0f * (1.0f + term_window_right->line_spacing.value);

    // Text content area inside window (respect margins + border padding)
    float border_padding = term_window_right->border_size.size * 2.0f;
    float usable_h = term_window_right->size.height
                   - term_window_right->text_margins.top
                   - term_window_right->text_margins.bottom
                   - border_padding;

    // Account for vertical padding for first line (0.75 * line_height)
    float vertical_padding = char_h * 0.75f;
    usable_h -= vertical_padding;

    int cols = terminal_cols_for_window(term_window_right);
    int rows = (int)(usable_h / char_h);

    // Subtract 3 rows total: 2 for safety + 1 to match rendering descender clipping
    if (rows > 3) rows -= 3;

    if (rows < 10) rows = 10;  // Ensure minimum rows for programs like top

    // Don't advertise more than we can store in ansi_term
    if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;

    // Keep the emulator's autowrap column in step with what the child is told.
    ansi_set_size(&ansi_term_right, cols, rows);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    DEBUG_PRINT("Right PTY size: %d rows x %d cols (usable height: %.0f, char height: %.1f)\n",
                rows, cols, usable_h, char_h);

    // Check if pty_master_fd_right is a valid terminal before ioctl
    if (isatty(pty_master_fd_right)) {
        if (ioctl(pty_master_fd_right, TIOCSWINSZ, &ws) < 0) {
            // Silently handle ioctl failures - PTY may not support it yet
            // or may be in an intermediate state
        }
        if (pty_child_pid_right > 0) {
            kill(pty_child_pid_right, SIGWINCH);
        }
    }
}

// Resize bottom PTY to match window
void resize_pty_to_window_bottom() {
    if (pty_master_fd_bottom < 0 || !term_window_bottom || !term_window_bottom->font) return;

    // Get actual character dimensions from font metrics
    float char_h = term_window_bottom->font_size.value * 16.0f * (1.0f + term_window_bottom->line_spacing.value);

    // Text content area inside window (respect margins + border padding)
    float border_padding = term_window_bottom->border_size.size * 2.0f;
    float usable_h = term_window_bottom->size.height
                   - term_window_bottom->text_margins.top
                   - term_window_bottom->text_margins.bottom
                   - border_padding;

    // Account for vertical padding for first line (0.75 * line_height)
    float vertical_padding = char_h * 0.75f;
    usable_h -= vertical_padding;

    int cols = terminal_cols_for_window(term_window_bottom);
    int rows = (int)(usable_h / char_h);

    // Subtract 3 rows total: 2 for safety + 1 to match rendering descender clipping
    if (rows > 3) rows -= 3;

    if (rows < 10) rows = 10;  // Ensure minimum rows for programs like top

    // Don't advertise more than we can store in ansi_term
    if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;

    // Keep the emulator's autowrap column in step with what the child is told.
    ansi_set_size(&ansi_term_bottom, cols, rows);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    DEBUG_PRINT("Bottom PTY size: %d rows x %d cols (usable height: %.0f, char height: %.1f)\n",
                rows, cols, usable_h, char_h);

    // Check if pty_master_fd_bottom is a valid terminal before ioctl
    if (isatty(pty_master_fd_bottom)) {
        if (ioctl(pty_master_fd_bottom, TIOCSWINSZ, &ws) < 0) {
            // Silently handle ioctl failures - PTY may not support it yet
            // or may be in an intermediate state
        }
        if (pty_child_pid_bottom > 0) {
            kill(pty_child_pid_bottom, SIGWINCH);
        }
    }
}

// Initialize terminal
void terminal_init() {
    terminal.line_count = 0;
    terminal.raw_line_count = 0;
    terminal.scroll_offset = 0;
    terminal.cursor_pos = 0;
    terminal.cursor_visible = true;
    terminal.cursor_blink_timer = 0.0f;
    terminal.input_buffer[0] = '\0';

    // Initialize ANSI terminal
    ansi_init(&ansi_term);

    // Initialize prompt
    terminal_update_prompt();

    // Load command history
    terminal_load_history();

    // Add welcome message
    terminal_add_line("Terminal Emulator v1.0");
    terminal_add_line("Type 'shell' to start a shell, 'help' for commands");
    terminal_add_line("Press Ctrl+Q to exit");
    terminal_add_line("========================================");
}

// Strip ANSI escape codes from text
void strip_ansi_codes(char* dest, const char* src, size_t max_len) {
    size_t j = 0;
    bool in_escape = false;

    for (size_t i = 0; src[i] != '\0' && j < max_len - 1; i++) {
        if (src[i] == '\033' || src[i] == '\x1b') {
            // Start of ANSI escape sequence
            in_escape = true;
            continue;
        }

        if (in_escape) {
            // Check for end of escape sequence
            if ((src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= 'a' && src[i] <= 'z')) {
                in_escape = false;
            }
            continue;
        }

        // Filter out other control characters except newline and tab
        if (src[i] < 32 && src[i] != '\n' && src[i] != '\t') {
            continue;
        }

        dest[j++] = src[i];
    }
    dest[j] = '\0';
}

// Calculate how many characters fit in one line based on window width
// OPTIMIZATION: Now uses cache to avoid repeated calculation
int calculate_line_width() {
    // Return cached value if available
    if (cached_line_width != -1) {
        return cached_line_width;
    }

    if (!term_window || !term_window->font) return 80;  // Default

    // Same width the renderer will draw at. Wrapping wider than that silently
    // truncated the overflow at render time.
    int chars_per_line = terminal_cols_for_window(term_window);

    // CRITICAL: Prevent buffer overflow - never exceed MAX_LINE_LENGTH
    if (chars_per_line >= MAX_LINE_LENGTH - 1) {
        chars_per_line = MAX_LINE_LENGTH - 2;
    }

    // Cache the result
    cached_line_width = chars_per_line;
    return chars_per_line;
}

// Invalidate cached line width (call on resize, font size change, etc.)
void invalidate_line_width_cache() {
    cached_line_width = -1;
}

// Word wrap a single raw line into multiple display lines
void wrap_line(const char* raw_line, char wrapped_lines[][MAX_LINE_LENGTH], int* wrapped_count, int max_wrapped, int max_width) {
    *wrapped_count = 0;
    int pos = 0;
    int line_len = strlen(raw_line);

    while (pos < line_len && *wrapped_count < max_wrapped) {
        int chars_to_copy = max_width;

        // Don't split in the middle of a word if possible
        if (pos + chars_to_copy < line_len) {
            // Look for last space within the line
            int last_space = -1;
            for (int i = chars_to_copy - 1; i >= 0; i--) {
                if (raw_line[pos + i] == ' ') {
                    last_space = i;
                    break;
                }
            }

            // If we found a space and it's not too far back, break there
            if (last_space > max_width / 2) {
                chars_to_copy = last_space + 1;  // Include the space
            }
        } else {
            chars_to_copy = line_len - pos;
        }

        // CRITICAL: Defensive clamp to prevent buffer overflow
        if (chars_to_copy >= MAX_LINE_LENGTH) {
            chars_to_copy = MAX_LINE_LENGTH - 1;
        }

        // Copy the line segment
        strncpy(wrapped_lines[*wrapped_count], &raw_line[pos], chars_to_copy);
        wrapped_lines[*wrapped_count][chars_to_copy] = '\0';

        // Trim leading/trailing spaces
        char* start = wrapped_lines[*wrapped_count];
        while (*start == ' ') start++;
        if (start != wrapped_lines[*wrapped_count]) {
            memmove(wrapped_lines[*wrapped_count], start, strlen(start) + 1);
        }

        (*wrapped_count)++;
        pos += chars_to_copy;
    }
}

// Rewrap all terminal content to fit current window width
// NOTE: This is expensive (O(N) where N = raw_line_count). Only call on:
// - Window resize
// - Font size change
// - Buffer overflow requiring scroll
// Normal line additions use incremental wrapping in terminal_add_line()
void rewrap_terminal_content() {
    if (!term_window) return;

    // Invalidate cache since window properties may have changed
    invalidate_line_width_cache();

    int max_width = calculate_line_width();

    // Clear display lines
    terminal.line_count = 0;

    // Process each raw line and wrap it
    for (int i = 0; i < terminal.raw_line_count; i++) {
        char temp_wrapped[10][MAX_LINE_LENGTH];
        int wrapped_count = 0;

        wrap_line(terminal.raw_lines[i], temp_wrapped, &wrapped_count, 10, max_width);

        // Add wrapped lines to display
        for (int j = 0; j < wrapped_count && terminal.line_count < MAX_TERMINAL_LINES; j++) {
            strcpy(terminal.lines[terminal.line_count], temp_wrapped[j]);
            terminal.line_count++;
        }
    }
}

// Add a line to terminal output
// OPTIMIZATION: Incremental wrapping - only wraps new line, not all lines
// Drop the oldest `count` display lines to make room at the end.
static void terminal_drop_oldest_lines(int count) {
    if (count <= 0) return;
    if (count >= terminal.line_count) {
        terminal.line_count = 0;
    } else {
        memmove(terminal.lines[0], terminal.lines[count],
                (size_t)(terminal.line_count - count) * MAX_LINE_LENGTH);
        terminal.line_count -= count;
    }
    terminal.scroll_offset -= count;
    if (terminal.scroll_offset < 0) terminal.scroll_offset = 0;
}

void terminal_add_line(const char* text) {
    // Scrollback is full: drop the oldest raw line to make room. This used to
    // return early, which threw away every line after the buffer filled up.
    if (terminal.raw_line_count >= MAX_TERMINAL_LINES) {
        memmove(terminal.raw_lines[0], terminal.raw_lines[1],
                (size_t)(MAX_TERMINAL_LINES - 1) * MAX_LINE_LENGTH);
        terminal.raw_line_count = MAX_TERMINAL_LINES - 1;
    }

    // Strip ANSI codes before adding
    char clean_text[MAX_LINE_LENGTH];
    strip_ansi_codes(clean_text, text, MAX_LINE_LENGTH);

    strncpy(terminal.raw_lines[terminal.raw_line_count], clean_text, MAX_LINE_LENGTH - 1);
    terminal.raw_lines[terminal.raw_line_count][MAX_LINE_LENGTH - 1] = '\0';
    terminal.raw_line_count++;

    // OPTIMIZATION: Only wrap the newly added line instead of rewrapping everything
    int max_width = calculate_line_width();

    char temp_wrapped[10][MAX_LINE_LENGTH];
    int wrapped_count = 0;
    wrap_line(terminal.raw_lines[terminal.raw_line_count - 1], temp_wrapped, &wrapped_count, 10, max_width);

    // Make room at the end rather than dropping the new content.
    if (terminal.line_count + wrapped_count > MAX_TERMINAL_LINES) {
        terminal_drop_oldest_lines(terminal.line_count + wrapped_count - MAX_TERMINAL_LINES);
    }

    for (int j = 0; j < wrapped_count && terminal.line_count < MAX_TERMINAL_LINES; j++) {
        strcpy(terminal.lines[terminal.line_count], temp_wrapped[j]);
        terminal.line_count++;
    }

    // Auto-scroll to bottom to show new content
    // Calculate how many lines can fit in the visible area
    if (term_window && term_window->font) {
        float line_height = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);
        float border_padding = term_window->border_size.size;
        float content_height = term_window->size.height - term_window->text_margins.top -
                              term_window->text_margins.bottom - (border_padding * 2);
        float available_height = content_height - line_height; // Reserve 1 line for input prompt
        int max_visible_lines = (int)(available_height / line_height);
        if (max_visible_lines < 1) max_visible_lines = 1;

        // Set scroll to show the most recent lines
        int max_scroll = terminal.line_count - max_visible_lines + 1;
        if (max_scroll < 0) max_scroll = 0;
        terminal.scroll_offset = max_scroll;
    }
}

// Execute terminal command
void terminal_execute_command(const char* cmd) {
    // Add command to history with bash-style prompt
    char cmd_line[MAX_LINE_LENGTH * 2];
    snprintf(cmd_line, sizeof(cmd_line), "%s%s", terminal.prompt, cmd);
    terminal_add_line(cmd_line);

    if (strlen(cmd) == 0) {
        terminal_update_prompt();  // Update prompt even for empty commands
        return;
    }

    // Add command to history array (skip empty commands and duplicates)
    if (strlen(cmd) > 0) {
        // Check if this is a duplicate of the last command
        bool is_duplicate = false;
        if (terminal.history_count > 0) {
            if (strcmp(terminal.history[terminal.history_count - 1], cmd) == 0) {
                is_duplicate = true;
            }
        }

        if (!is_duplicate) {
            // Add to history
            if (terminal.history_count >= MAX_TERMINAL_LINES) {
                // Shift history up to make room
                for (int i = 0; i < MAX_TERMINAL_LINES - 1; i++) {
                    strcpy(terminal.history[i], terminal.history[i + 1]);
                }
                terminal.history_count = MAX_TERMINAL_LINES - 1;
            }

            strncpy(terminal.history[terminal.history_count], cmd, MAX_LINE_LENGTH - 1);
            terminal.history[terminal.history_count][MAX_LINE_LENGTH - 1] = '\0';
            terminal.history_count++;
        }
    }

    // Handle built-in commands
    if (strcmp(cmd, "help") == 0) {
        terminal_add_line("Available commands:");
        terminal_add_line("  help    - Show this help");
        terminal_add_line("  clear   - Clear terminal");
        terminal_add_line("  echo    - Echo text");
        terminal_add_line("  exit    - Exit terminal");
        terminal_add_line("  shell   - Start interactive bash shell");
        terminal_add_line("  ls      - List files");
        terminal_add_line("  pwd     - Print working directory");
    } else if (strcmp(cmd, "clear") == 0) {
        terminal.line_count = 0;
        terminal.raw_line_count = 0;
    } else if (strcmp(cmd, "exit") == 0) {
        exit(0);
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        terminal_add_line(cmd + 5);
    } else if (strcmp(cmd, "pwd") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            terminal_add_line(cwd);
        } else {
            terminal_add_line("Error getting current directory");
        }
    } else if (strncmp(cmd, "cd ", 3) == 0 || strcmp(cmd, "cd") == 0) {
        // Handle cd command
        const char* path = (strlen(cmd) > 3) ? cmd + 3 : getenv("HOME");

        // Expand tilde (~) to home directory
        char expanded_path[1024];
        if (path && path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                if (path[1] == '\0') {
                    // Just "~"
                    snprintf(expanded_path, sizeof(expanded_path), "%s", home);
                } else if (path[1] == '/') {
                    // "~/something"
                    snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, path + 1);
                } else {
                    // "~username" - not supported, use as-is
                    strncpy(expanded_path, path, sizeof(expanded_path) - 1);
                    expanded_path[sizeof(expanded_path) - 1] = '\0';
                }
                path = expanded_path;
            }
        }

        if (path && chdir(path) == 0) {
            terminal_update_prompt();  // Update prompt after changing directory
        } else {
            terminal_add_line("cd: no such file or directory");
        }
    } else if (strcmp(cmd, "ls") == 0 || strncmp(cmd, "ls ", 3) == 0) {
        // Execute ls command
        FILE* pipe = popen(cmd, "r");
        if (pipe) {
            char buffer[MAX_LINE_LENGTH];
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                // Remove newline
                buffer[strcspn(buffer, "\n")] = 0;
                terminal_add_line(buffer);
            }
            pclose(pipe);
        } else {
            terminal_add_line("Error executing command");
        }
    } else if (strcmp(cmd, "shell") == 0) {
        // Start interactive bash shell
        start_interactive_shell(NULL);
        return;
    } else if (strcmp(cmd, "top") == 0) {
        // Start top in interactive mode
        start_interactive_shell("top");
        return;
    } else if (strcmp(cmd, "htop") == 0) {
        start_interactive_shell("htop");
        return;
    } else if (strncmp(cmd, "vim ", 4) == 0 || strcmp(cmd, "vim") == 0) {
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "vi ", 3) == 0 || strcmp(cmd, "vi") == 0) {
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "nano ", 5) == 0 || strcmp(cmd, "nano") == 0) {
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "less ", 5) == 0 || strcmp(cmd, "less") == 0) {
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "more ", 5) == 0 || strcmp(cmd, "more") == 0) {
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "ssh ", 4) == 0 || strcmp(cmd, "ssh") == 0) {
        // ssh needs interactive PTY for password prompts
        start_interactive_shell(cmd);
        return;
    } else if (strncmp(cmd, "sudo ", 5) == 0) {
        // sudo needs interactive PTY for password prompts
        start_interactive_shell(cmd);
        return;
    } else if (strcmp(cmd, "python") == 0 || strcmp(cmd, "python3") == 0 ||
               strncmp(cmd, "python ", 7) == 0 || strncmp(cmd, "python3 ", 8) == 0) {
        // python REPL or scripts may need interactive input
        start_interactive_shell(cmd);
        return;
    } else {
        // Handle non-interactive commands with popen
        char modified_cmd[MAX_LINE_LENGTH * 2];
        strncpy(modified_cmd, cmd, sizeof(modified_cmd) - 1);
        modified_cmd[sizeof(modified_cmd) - 1] = '\0';

        // Try to execute as shell command (with ANSI support)
        FILE* pipe = popen(modified_cmd, "r");
        if (pipe) {
            char buffer[4096];
            bool output_found = false;

            // Clear ANSI terminal for new command output
            ansi_clear(&ansi_term);

            // Read all output and process through ANSI terminal
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                ansi_process_output(&ansi_term, buffer, strlen(buffer));
                output_found = true;
            }

            int status = pclose(pipe);

            // Convert ANSI buffer to display lines
            if (output_found) {
                for (int row = 0; row < ANSI_BUFFER_ROWS; row++) {
                    char line[MAX_LINE_LENGTH];
                    ansi_get_line(&ansi_term, row, line, MAX_LINE_LENGTH);
                    if (strlen(line) > 0) {
                        terminal_add_line(line);
                    }
                }
            }

            if (!output_found && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                char error[MAX_LINE_LENGTH];
                snprintf(error, sizeof(error), "Command failed with status %d", WEXITSTATUS(status));
                terminal_add_line(error);
            }
        } else {
            terminal_add_line("Unknown command. Type 'help' for available commands.");
        }
    }
}

// Tab completion function
// Rebuild the line up to the point the completion is appended: everything
// before the word being completed, plus that word's directory part if it has
// one. Both completion branches need exactly this, and both used to build it
// inline with unbounded strncat calls.
static void tab_build_line_head(char* out, size_t out_size, const char* buffer,
                                int word_offset, const char* word_start, bool is_path) {
    size_t head = (size_t)word_offset;
    if (head >= out_size) head = out_size - 1;
    memcpy(out, buffer, head);
    out[head] = '\0';

    if (!is_path) return;

    const char* last_slash = strrchr(word_start, '/');
    if (!last_slash) return;

    size_t dir_len = (size_t)(last_slash - word_start) + 1;   // keep the slash
    size_t room = out_size - strlen(out) - 1;
    if (dir_len > room) dir_len = room;
    strncat(out, word_start, dir_len);
}

void handle_tab_completion() {
    double current_time = glfwGetTime();
    bool is_double_tab = (current_time - last_tab_time) < 0.5;
    last_tab_time = current_time;

    // Special case: double-tab on empty input or after directory - list current directory
    if (terminal.cursor_pos == 0 ||
        (is_double_tab && strlen(terminal.input_buffer) > 0 &&
         terminal.input_buffer[terminal.cursor_pos - 1] == '/')) {

        // List current directory or the directory that was just completed
        const char* list_dir = ".";
        if (is_double_tab && strlen(last_completed_path) > 0) {
            list_dir = last_completed_path;
        }

        DIR* dir = opendir(list_dir);
        if (dir) {
            terminal_add_line("");
            terminal_add_line("Contents:");

            struct dirent* entry;
            char line[MAX_LINE_LENGTH] = "";
            int count = 0;

            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;  // Skip hidden files

                // Add directory indicator
                struct stat st;
                // list_dir may be last_completed_path (2 * MAX_LINE_LENGTH),
                // plus a separator and a directory entry name.
                char full_path[MAX_LINE_LENGTH * 4];
                snprintf(full_path, sizeof(full_path), "%s/%s", list_dir, entry->d_name);
                bool is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));

                char item[32];
                snprintf(item, sizeof(item), "%-25.25s%s", entry->d_name, is_dir ? "/" : " ");

                if (count % 3 == 0) {
                    if (count > 0) terminal_add_line(line);
                    snprintf(line, sizeof(line), "  %s", item);
                } else {
                    strncat(line, item, sizeof(line) - strlen(line) - 1);
                }
                count++;
            }
            if (count > 0) terminal_add_line(line);
            closedir(dir);

            // Re-display prompt
            char echo_line[MAX_LINE_LENGTH * 2];
            snprintf(echo_line, sizeof(echo_line), "%s%s", terminal.prompt, terminal.input_buffer);
            terminal_add_line(echo_line);
        }
        return;
    }

    char input[MAX_LINE_LENGTH];
    strncpy(input, terminal.input_buffer, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    // Find the word to complete (last space-separated token)
    char* last_space = strrchr(input, ' ');
    char* word_start = last_space ? last_space + 1 : input;
    int word_offset = word_start - input;

    if (*word_start == '\0') return;  // Nothing to complete

    // Check if completing a path (contains /)
    bool is_path = strchr(word_start, '/') != NULL;
    char dir_path[MAX_LINE_LENGTH] = ".";
    char prefix[MAX_LINE_LENGTH] = "";

    if (is_path) {
        // Split into directory and prefix
        char temp[MAX_LINE_LENGTH];
        strncpy(temp, word_start, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';  // Ensure NUL termination
        char* last_slash = strrchr(temp, '/');
        if (last_slash) {
            *last_slash = '\0';
            strncpy(dir_path, temp[0] ? temp : "/", sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0';  // Ensure NUL termination
            strncpy(prefix, last_slash + 1, sizeof(prefix) - 1);
            prefix[sizeof(prefix) - 1] = '\0';  // Ensure NUL termination
        }
    } else {
        strncpy(prefix, word_start, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';  // Ensure NUL termination
    }

    // Find matching files/directories
    DIR* dir = opendir(dir_path);
    if (!dir) return;

    char matches[100][MAX_LINE_LENGTH];
    int match_count = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL && match_count < 100) {
        if (entry->d_name[0] == '.' && prefix[0] != '.') continue;  // Skip hidden unless explicitly typed

        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            strncpy(matches[match_count], entry->d_name, MAX_LINE_LENGTH - 1);
            matches[match_count][MAX_LINE_LENGTH - 1] = '\0';  // Ensure NUL termination

            // Add / for directories
            struct stat st;
            char full_path[MAX_LINE_LENGTH * 2];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncat(matches[match_count], "/", MAX_LINE_LENGTH - strlen(matches[match_count]) - 1);
            }
            match_count++;
        }
    }
    closedir(dir);

    if (match_count == 0) {
        return;  // No matches
    } else if (match_count == 1) {
        // Single match - complete it
        char new_input[MAX_LINE_LENGTH];
        tab_build_line_head(new_input, sizeof(new_input), terminal.input_buffer,
                            word_offset, word_start, is_path);

        strncat(new_input, matches[0], sizeof(new_input) - strlen(new_input) - 1);

        // Store completed path if it's a directory (for double-tab listing)
        if (matches[0][strlen(matches[0]) - 1] == '/') {
            // Build full path for directory
            if (is_path) {
                snprintf(last_completed_path, sizeof(last_completed_path), "%s/%s", dir_path, matches[0]);
            } else {
                snprintf(last_completed_path, sizeof(last_completed_path), "./%s", matches[0]);
            }
            // Remove trailing slash for opendir
            size_t len = strlen(last_completed_path);
            if (len > 0 && last_completed_path[len - 1] == '/') {
                last_completed_path[len - 1] = '\0';
            }
        } else {
            last_completed_path[0] = '\0';
        }

        strncpy(terminal.input_buffer, new_input, MAX_LINE_LENGTH - 1);
        terminal.input_buffer[MAX_LINE_LENGTH - 1] = '\0';  // Ensure NUL termination
        terminal.cursor_pos = strlen(terminal.input_buffer);
        strncpy(last_tab_completion, terminal.input_buffer, sizeof(last_tab_completion) - 1);
        last_tab_completion[sizeof(last_tab_completion) - 1] = '\0';  // Ensure NUL termination

        // Echo completion with prompt
        char echo_line[MAX_LINE_LENGTH * 2];
        snprintf(echo_line, sizeof(echo_line), "%s%s", terminal.prompt, terminal.input_buffer);
        terminal_add_line(echo_line);
    } else {
        // Multiple matches
        // Show all matches on first tab or double-tab
        terminal_add_line("");
        char line[MAX_LINE_LENGTH];
        for (int i = 0; i < match_count; i++) {
            if (i % 3 == 0) {
                if (i > 0) terminal_add_line(line);
                snprintf(line, sizeof(line), "  %-25.25s", matches[i]);
            } else {
                char temp[30];
                snprintf(temp, sizeof(temp), "%-25.25s", matches[i]);
                strncat(line, temp, MAX_LINE_LENGTH - strlen(line) - 1);
            }
        }
        if (match_count > 0) terminal_add_line(line);

        // Find common prefix
        int common_len = strlen(prefix);
        for (int pos = common_len; matches[0][pos]; pos++) {
            bool all_match = true;
            for (int i = 1; i < match_count; i++) {
                if (matches[i][pos] != matches[0][pos]) {
                    all_match = false;
                    break;
                }
            }
            if (!all_match) break;
            common_len++;
        }

        // Complete to common prefix
        if (common_len > (int)strlen(prefix)) {
            char new_input[MAX_LINE_LENGTH];
            tab_build_line_head(new_input, sizeof(new_input), terminal.input_buffer,
                                word_offset, word_start, is_path);

            // Append only the common prefix. There used to be an explicit
            // NUL at new_input[word_offset + common_len] here, which ignored
            // the directory part appended above: for a path completion it
            // truncated the line straight back to the directory, deleting
            // what had been typed instead of extending it.
            size_t room = sizeof(new_input) - strlen(new_input) - 1;
            size_t take = (size_t)common_len < room ? (size_t)common_len : room;
            strncat(new_input, matches[0], take);

            strncpy(terminal.input_buffer, new_input, MAX_LINE_LENGTH - 1);
            terminal.input_buffer[MAX_LINE_LENGTH - 1] = '\0';  // Ensure NUL termination
            terminal.cursor_pos = strlen(terminal.input_buffer);
        }

        // Store last completion state
        strncpy(last_tab_completion, terminal.input_buffer, sizeof(last_tab_completion) - 1);
        last_tab_completion[sizeof(last_tab_completion) - 1] = '\0';  // Ensure NUL termination

        // Re-display prompt
        char echo_line[MAX_LINE_LENGTH * 2];
        snprintf(echo_line, sizeof(echo_line), "%s%s", terminal.prompt, terminal.input_buffer);
        terminal_add_line(echo_line);
    }
}

// Keyboard input callback
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

    // Enter blip. Only on the initial press - holding Enter down repeats the
    // key, and retriggering the clip on every repeat would machine-gun it.
    if (action == GLFW_PRESS && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)) {
        sound_play();
    }

    // Context menu text positioning with arrow keys
    if (context_menu.visible) {
        if (key == GLFW_KEY_UP) {
            context_menu.text_y_offset -= 1.0f;
            printf("Menu text Y offset: %.1f\n", context_menu.text_y_offset);
            return;
        }
        if (key == GLFW_KEY_DOWN) {
            context_menu.text_y_offset += 1.0f;
            printf("Menu text Y offset: %.1f\n", context_menu.text_y_offset);
            return;
        }
    }

    // Ctrl+Q to exit terminal
    if (key == GLFW_KEY_Q && (mods & GLFW_MOD_CONTROL)) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }

    // Common terminal control sequences (only in non-interactive mode)
    if (!interactive_mode && (mods & GLFW_MOD_CONTROL)) {
        if (key == GLFW_KEY_A) {
            // Ctrl+A: Move to beginning of line
            terminal.cursor_pos = 0;
            return;
        }
        if (key == GLFW_KEY_E) {
            // Ctrl+E: Move to end of line
            terminal.cursor_pos = strlen(terminal.input_buffer);
            return;
        }
        if (key == GLFW_KEY_U) {
            // Ctrl+U: Clear line (delete from cursor to beginning)
            int len = strlen(terminal.input_buffer);
            if (terminal.cursor_pos > 0 && terminal.cursor_pos <= len) {
                int remaining = len - terminal.cursor_pos;
                memmove(terminal.input_buffer, terminal.input_buffer + terminal.cursor_pos, remaining + 1);
                terminal.cursor_pos = 0;
            }
            return;
        }
        if (key == GLFW_KEY_K) {
            // Ctrl+K: Kill line (delete from cursor to end)
            int len = strlen(terminal.input_buffer);
            if (terminal.cursor_pos >= 0 && terminal.cursor_pos <= len) {
                terminal.input_buffer[terminal.cursor_pos] = '\0';
            }
            return;
        }
        if (key == GLFW_KEY_W) {
            // Ctrl+W: Delete word backwards
            int len = strlen(terminal.input_buffer);
            if (terminal.cursor_pos > 0 && terminal.cursor_pos <= len) {
                int start = terminal.cursor_pos;
                // Skip trailing spaces
                while (start > 0 && terminal.input_buffer[start - 1] == ' ') {
                    start--;
                }
                // Delete word
                while (start > 0 && terminal.input_buffer[start - 1] != ' ') {
                    start--;
                }
                int remaining = len - terminal.cursor_pos;
                memmove(terminal.input_buffer + start, terminal.input_buffer + terminal.cursor_pos, remaining + 1);
                terminal.cursor_pos = start;
            }
            return;
        }
        if (key == GLFW_KEY_L) {
            // Ctrl+L: Clear screen
            terminal.line_count = 0;
            terminal.raw_line_count = 0;
            terminal.scroll_offset = 0;
            return;
        }
    }

    // F4-F7: Adjust margins dynamically
    if (key == GLFW_KEY_F4) {
        term_window->text_margins.top += 1;
        printf("Top margin: %d (text moved down)\n", (int)term_window->text_margins.top);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }
    if (key == GLFW_KEY_F5) {
        term_window->text_margins.top -= 1;
        printf("Top margin: %d (text moved up)\n", (int)term_window->text_margins.top);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }
    if (key == GLFW_KEY_F6) {
        term_window->text_margins.bottom += 1;
        printf("Bottom margin: %d (extended)\n", (int)term_window->text_margins.bottom);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }
    if (key == GLFW_KEY_F7) {
        term_window->text_margins.bottom -= 1;
        printf("Bottom margin: %d (reduced)\n", (int)term_window->text_margins.bottom);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }
    if (key == GLFW_KEY_F8) {
        // Increase font size in focused terminal
        if (split_horizontal && focused_terminal == 1) {
            term_window_right->font_size.value += 0.1f;
            if (term_window_right->font_size.value > 5.0f) term_window_right->font_size.value = 5.0f;
            printf("Right terminal font size: %.1f (%.0fpx)\n", term_window_right->font_size.value, term_window_right->font_size.value * 16.0f);
            fflush(stdout);
            resize_pty_to_window_right();
        } else {
            term_window->font_size.value += 0.1f;
            if (term_window->font_size.value > 5.0f) term_window->font_size.value = 5.0f;
            printf("Left terminal font size: %.1f (%.0fpx)\n", term_window->font_size.value, term_window->font_size.value * 16.0f);
            fflush(stdout);
            resize_pty_to_window();
            rewrap_terminal_content();
        }
        return;
    }
    if (key == GLFW_KEY_F9) {
        // Decrease font size in focused terminal
        if (split_horizontal && focused_terminal == 1) {
            term_window_right->font_size.value -= 0.1f;
            if (term_window_right->font_size.value < 0.5f) term_window_right->font_size.value = 0.5f;
            printf("Right terminal font size: %.1f (%.0fpx)\n", term_window_right->font_size.value, term_window_right->font_size.value * 16.0f);
            fflush(stdout);
            resize_pty_to_window_right();
        } else {
            term_window->font_size.value -= 0.1f;
            if (term_window->font_size.value < 0.5f) term_window->font_size.value = 0.5f;
            printf("Left terminal font size: %.1f (%.0fpx)\n", term_window->font_size.value, term_window->font_size.value * 16.0f);
            fflush(stdout);
            resize_pty_to_window();
            rewrap_terminal_content();
        }
        return;
    }
    if (key == GLFW_KEY_F10) {
        term_window->text_margins.left += 1;
        printf("Left margin: %d (text moved right)\n", (int)term_window->text_margins.left);
        fflush(stdout);
        resize_pty_to_window();
        rewrap_terminal_content();  // Margin change affects line width
        return;
    }
    if (key == GLFW_KEY_F11) {
        term_window->text_margins.left -= 1;
        printf("Left margin: %d (text moved left)\n", (int)term_window->text_margins.left);
        fflush(stdout);
        resize_pty_to_window();
        rewrap_terminal_content();  // Margin change affects line width
        return;
    }

    // Control key handling for interactive mode (Ctrl+C, Ctrl+D, Ctrl+Z)
    if (mods & GLFW_MOD_CONTROL) {
        char control_char = 0;

        if (key == GLFW_KEY_C) {
            control_char = 0x03;  // Ctrl+C (SIGINT)
        } else if (key == GLFW_KEY_D) {
            control_char = 0x04;  // Ctrl+D (EOF)
        } else if (key == GLFW_KEY_Z) {
            control_char = 0x1A;  // Ctrl+Z (SIGTSTP)
        }

        if (control_char != 0) {
            // Route to appropriate terminal in split mode
            if (split_horizontal) {
                if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
                    write(pty_master_fd, &control_char, 1);
                    return;
                } else if (focused_terminal == 1 && interactive_mode_right && pty_master_fd_right >= 0) {
                    write(pty_master_fd_right, &control_char, 1);
                    return;
                }
            } else if (split_vertical) {
                if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
                    write(pty_master_fd, &control_char, 1);
                    return;
                } else if (focused_terminal == 1 && interactive_mode_bottom && pty_master_fd_bottom >= 0) {
                    write(pty_master_fd_bottom, &control_char, 1);
                    return;
                }
            } else if (interactive_mode && pty_master_fd >= 0) {
                write(pty_master_fd, &control_char, 1);
                return;
            }
        }
    }

    // Interactive mode - send all keys to PTY (including ESC for vim)
    // In split mode, route to focused terminal
    if (split_horizontal) {
        if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
            send_key_to_pty(key);
            return;
        } else if (focused_terminal == 1 && interactive_mode_right && pty_master_fd_right >= 0) {
            // Send to right terminal PTY
            const char *seq = NULL;
            char c;
            ssize_t result;
            int fd = pty_master_fd_right;

            switch (key) {
                case GLFW_KEY_ESCAPE:
                    c = 0x1b;
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to right pty");
                    }
                    return;
                case GLFW_KEY_ENTER:
                    c = '\r';
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to right pty");
                    }
                    return;
                case GLFW_KEY_BACKSPACE:
                    c = 0x7f;
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to right pty");
                    }
                    return;
                case GLFW_KEY_TAB:
                    c = '\t';
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to right pty");
                    }
                    return;

                // Arrow keys
                case GLFW_KEY_UP:    seq = "\x1b[A"; break;
                case GLFW_KEY_DOWN:  seq = "\x1b[B"; break;
                case GLFW_KEY_RIGHT: seq = "\x1b[C"; break;
                case GLFW_KEY_LEFT:  seq = "\x1b[D"; break;

                // Home/End/Page/Delete keys
                case GLFW_KEY_HOME:      seq = "\x1b[H"; break;
                case GLFW_KEY_END:       seq = "\x1b[F"; break;
                case GLFW_KEY_PAGE_UP:   seq = "\x1b[5~"; break;
                case GLFW_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
                case GLFW_KEY_DELETE:    seq = "\x1b[3~"; break;
            }

            if (seq) {
                result = write(fd, seq, strlen(seq));
                if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write to right pty");
                }
            }
            return;
        }
    } else if (split_vertical) {
        if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
            send_key_to_pty(key);
            return;
        } else if (focused_terminal == 1 && interactive_mode_bottom && pty_master_fd_bottom >= 0) {
            // Send to bottom terminal PTY
            const char *seq = NULL;
            char c;
            ssize_t result;
            int fd = pty_master_fd_bottom;

            switch (key) {
                case GLFW_KEY_ESCAPE:
                    c = 0x1b;
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to bottom pty");
                    }
                    return;
                case GLFW_KEY_ENTER:
                    c = '\r';
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to bottom pty");
                    }
                    return;
                case GLFW_KEY_BACKSPACE:
                    c = 0x7f;
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to bottom pty");
                    }
                    return;
                case GLFW_KEY_TAB:
                    c = '\t';
                    result = write(fd, &c, 1);
                    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("write to bottom pty");
                    }
                    return;

                // Arrow keys
                case GLFW_KEY_UP:    seq = "\x1b[A"; break;
                case GLFW_KEY_DOWN:  seq = "\x1b[B"; break;
                case GLFW_KEY_RIGHT: seq = "\x1b[C"; break;
                case GLFW_KEY_LEFT:  seq = "\x1b[D"; break;

                // Home/End/Page/Delete keys
                case GLFW_KEY_HOME:      seq = "\x1b[H"; break;
                case GLFW_KEY_END:       seq = "\x1b[F"; break;
                case GLFW_KEY_PAGE_UP:   seq = "\x1b[5~"; break;
                case GLFW_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
                case GLFW_KEY_DELETE:    seq = "\x1b[3~"; break;
            }

            if (seq) {
                result = write(fd, seq, strlen(seq));
                if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write to bottom pty");
                }
            }
            return;
        }
    } else if (interactive_mode && pty_master_fd >= 0) {
        // Single terminal mode
        send_key_to_pty(key);
        return;
    }

    // Non-interactive mode - original behavior
    if (key == GLFW_KEY_TAB) {
        handle_tab_completion();
        return;
    }

    if (key == GLFW_KEY_ENTER) {
        // Execute command
        terminal_execute_command(terminal.input_buffer);
        terminal.input_buffer[0] = '\0';
        terminal.cursor_pos = 0;
        terminal.history_index = -1;  // Reset history browsing

        // Auto-scroll to bottom after command execution
        if (term_window && term_window->font) {
            float line_height = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);
            int max_visible_lines = (int)((term_window->size.height - term_window->text_margins.top - term_window->text_margins.bottom) / line_height);
            int max_scroll = terminal.line_count - max_visible_lines + 1;
            if (max_scroll < 0) max_scroll = 0;
            terminal.scroll_offset = max_scroll;
        }
        return;
    }

    if (key == GLFW_KEY_LEFT) {
        // Move cursor left
        if (terminal.cursor_pos > 0) {
            terminal.cursor_pos--;
        }
        return;
    }

    if (key == GLFW_KEY_RIGHT) {
        // Move cursor right
        if (terminal.cursor_pos < (int)strlen(terminal.input_buffer)) {
            terminal.cursor_pos++;
        }
        return;
    }

    if (key == GLFW_KEY_BACKSPACE) {
        int len = strlen(terminal.input_buffer);
        // Ensure cursor position is valid
        if (terminal.cursor_pos > 0 && terminal.cursor_pos <= len) {
            // Delete character before cursor
            memmove(terminal.input_buffer + terminal.cursor_pos - 1,
                   terminal.input_buffer + terminal.cursor_pos,
                   len - terminal.cursor_pos + 1);
            terminal.cursor_pos--;
        }
        return;
    }

    if (key == GLFW_KEY_DELETE) {
        // Delete character at cursor
        int len = strlen(terminal.input_buffer);
        // Ensure cursor position is valid
        if (terminal.cursor_pos >= 0 && terminal.cursor_pos < len) {
            memmove(terminal.input_buffer + terminal.cursor_pos,
                   terminal.input_buffer + terminal.cursor_pos + 1,
                   len - terminal.cursor_pos);
        }
        return;
    }

    if (key == GLFW_KEY_HOME) {
        // Move to beginning of line
        terminal.cursor_pos = 0;
        return;
    }

    if (key == GLFW_KEY_END) {
        // Move to end of line
        terminal.cursor_pos = strlen(terminal.input_buffer);
        return;
    }

    if (key == GLFW_KEY_UP) {
        // Navigate command history backwards
        if (terminal.history_count == 0) {
            printf("No command history available\n");
            return;
        }

        if (terminal.history_index == -1) {
            // First time pressing up - save current input
            strncpy(terminal.history_temp, terminal.input_buffer, MAX_LINE_LENGTH - 1);
            terminal.history_temp[MAX_LINE_LENGTH - 1] = '\0';
            terminal.history_index = terminal.history_count - 1;
        } else if (terminal.history_index > 0) {
            terminal.history_index--;
        }

        // Load history entry into input buffer
        strncpy(terminal.input_buffer, terminal.history[terminal.history_index], MAX_LINE_LENGTH - 1);
        terminal.input_buffer[MAX_LINE_LENGTH - 1] = '\0';
        terminal.cursor_pos = strlen(terminal.input_buffer);

        // Visual feedback
        printf("History [%d/%d]: %s\n", terminal.history_index + 1, terminal.history_count, terminal.input_buffer);
        return;
    }

    if (key == GLFW_KEY_DOWN) {
        // Navigate command history forwards
        if (terminal.history_index == -1) return;  // Not browsing history

        terminal.history_index++;

        if (terminal.history_index >= terminal.history_count) {
            // Reached end - restore original input
            strncpy(terminal.input_buffer, terminal.history_temp, MAX_LINE_LENGTH - 1);
            terminal.input_buffer[MAX_LINE_LENGTH - 1] = '\0';
            terminal.history_index = -1;
        } else {
            // Load history entry
            strncpy(terminal.input_buffer, terminal.history[terminal.history_index], MAX_LINE_LENGTH - 1);
            terminal.input_buffer[MAX_LINE_LENGTH - 1] = '\0';
        }

        terminal.cursor_pos = strlen(terminal.input_buffer);
        return;
    }

    if (key == GLFW_KEY_PAGE_UP) {
        // In split mode, scroll the focused terminal
        if (split_horizontal && focused_terminal == 1) {
            terminal_right.scroll_offset = (terminal_right.scroll_offset > 10) ? terminal_right.scroll_offset - 10 : 0;
        } else {
            terminal.scroll_offset = (terminal.scroll_offset > 10) ? terminal.scroll_offset - 10 : 0;
        }
        return;
    }

    if (key == GLFW_KEY_PAGE_DOWN) {
        // In split mode, scroll the focused terminal
        if (split_horizontal && focused_terminal == 1) {
            // Calculate max visible lines for right terminal
            if (term_window_right && term_window_right->font) {
                float line_height = term_window_right->font_size.value * 16.0f * (1.0f + term_window_right->line_spacing.value);
                int max_visible_lines = (int)((term_window_right->size.height - term_window_right->text_margins.top - term_window_right->text_margins.bottom) / line_height);
                int max_scroll = terminal_right.line_count - max_visible_lines + 1;
                if (max_scroll < 0) max_scroll = 0;

                terminal_right.scroll_offset += 10;
                if (terminal_right.scroll_offset > max_scroll) {
                    terminal_right.scroll_offset = max_scroll;
                }
            }
        } else {
            // Calculate max visible lines for left terminal
            if (term_window && term_window->font) {
                float line_height = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);
                int max_visible_lines = (int)((term_window->size.height - term_window->text_margins.top - term_window->text_margins.bottom) / line_height);
                int max_scroll = terminal.line_count - max_visible_lines + 1;
                if (max_scroll < 0) max_scroll = 0;

                terminal.scroll_offset += 10;
                if (terminal.scroll_offset > max_scroll) {
                    terminal.scroll_offset = max_scroll;
                }
            }
        }
        return;
    }
}

// Character input callback
void char_callback(GLFWwindow* window, unsigned int codepoint) {
    (void)window;

    // In split mode, route character input to focused terminal
    if (split_horizontal) {
        char utf8[4];
        int len = codepoint_to_utf8(codepoint, utf8);

        if (len == 0) {
            return; // Invalid codepoint
        }

        if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
            ssize_t result = write(pty_master_fd, utf8, len);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (errno == EIO || errno == EBADF) {
                    close(pty_master_fd);
                    pty_master_fd = -1;
                    interactive_mode = false;
                    terminal_add_line("[pty closed]");
                }
            }
            return;
        } else if (focused_terminal == 1 && interactive_mode_right && pty_master_fd_right >= 0) {
            ssize_t result = write(pty_master_fd_right, utf8, len);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (errno == EIO || errno == EBADF) {
                    close(pty_master_fd_right);
                    pty_master_fd_right = -1;
                    interactive_mode_right = false;
                }
            }
            return;
        }
    } else if (split_vertical) {
        char utf8[4];
        int len = codepoint_to_utf8(codepoint, utf8);

        if (len == 0) {
            return; // Invalid codepoint
        }

        if (focused_terminal == 0 && interactive_mode && pty_master_fd >= 0) {
            ssize_t result = write(pty_master_fd, utf8, len);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (errno == EIO || errno == EBADF) {
                    close(pty_master_fd);
                    pty_master_fd = -1;
                    interactive_mode = false;
                    terminal_add_line("[pty closed]");
                }
            }
            return;
        } else if (focused_terminal == 1 && interactive_mode_bottom && pty_master_fd_bottom >= 0) {
            ssize_t result = write(pty_master_fd_bottom, utf8, len);
            if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (errno == EIO || errno == EBADF) {
                    close(pty_master_fd_bottom);
                    pty_master_fd_bottom = -1;
                    interactive_mode_bottom = false;
                }
            }
            return;
        }
    } else if (interactive_mode && pty_master_fd >= 0) {
        // Single terminal mode
        char utf8[4];
        int len = codepoint_to_utf8(codepoint, utf8);

        if (len == 0) {
            return; // Invalid codepoint
        }

        ssize_t result = write(pty_master_fd, utf8, len);
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // PTY might have closed
            if (errno == EIO || errno == EBADF) {
                close(pty_master_fd);
                pty_master_fd = -1;
                interactive_mode = false;
                terminal_add_line("[pty closed]");
            }
        }
        return;
    }

    // Insert character at cursor position (non-interactive mode)
    int len = strlen(terminal.input_buffer);

    // Bounds check: ensure we have room for new char + null terminator
    // and cursor position is valid
    if (len >= MAX_LINE_LENGTH - 1) {
        // Buffer full - silently ignore (terminal behavior)
        return;
    }

    if (terminal.cursor_pos < 0 || terminal.cursor_pos > len) {
        // Invalid cursor position - reset it
        terminal.cursor_pos = len;
    }

    // Make room for new character by shifting everything after cursor right
    // Safety: len < MAX_LINE_LENGTH - 1, so len + 1 < MAX_LINE_LENGTH (room for char + null)
    memmove(terminal.input_buffer + terminal.cursor_pos + 1,
           terminal.input_buffer + terminal.cursor_pos,
           len - terminal.cursor_pos + 1);  // +1 to include null terminator

    // Insert character (only ASCII printable for now)
    terminal.input_buffer[terminal.cursor_pos] = (char)codepoint;
    terminal.cursor_pos++;
    terminal.history_index = -1;  // Reset history browsing when typing

    // Ensure null termination (defensive)
    terminal.input_buffer[len + 1] = '\0';
}

// xterm-style 16-colour ANSI palette, normalised to 0..1.
// Indices 0-7 are the normal colours, 8-15 the bright ones. Blue is lifted off
// the classic (0,0,238) because that is close to unreadable on a dark ground.
static const float ansi_palette[ANSI_COLOR_COUNT][3] = {
    {0.00f, 0.00f, 0.00f},  // 0  black
    {0.80f, 0.00f, 0.00f},  // 1  red
    {0.00f, 0.80f, 0.00f},  // 2  green
    {0.80f, 0.80f, 0.00f},  // 3  yellow
    {0.25f, 0.35f, 1.00f},  // 4  blue
    {0.80f, 0.00f, 0.80f},  // 5  magenta
    {0.00f, 0.80f, 0.80f},  // 6  cyan
    {0.90f, 0.90f, 0.90f},  // 7  white (default foreground)
    {0.50f, 0.50f, 0.50f},  // 8  bright black (grey)
    {1.00f, 0.33f, 0.33f},  // 9  bright red
    {0.33f, 1.00f, 0.33f},  // 10 bright green
    {1.00f, 1.00f, 0.33f},  // 11 bright yellow
    {0.45f, 0.60f, 1.00f},  // 12 bright blue
    {1.00f, 0.33f, 1.00f},  // 13 bright magenta
    {0.33f, 1.00f, 1.00f},  // 14 bright cyan
    {1.00f, 1.00f, 1.00f},  // 15 bright white
};

// Look up an ANSI colour index. Bold promotes the normal colours to their
// bright counterparts, which is what most terminals do.
static void ansi_color_rgb(unsigned char index, bool bold, float* r, float* g, float* b) {
    if (bold && index < 8) index += 8;
    if (index >= ANSI_COLOR_COUNT) index = ANSI_DEFAULT_FG;
    *r = ansi_palette[index][0];
    *g = ansi_palette[index][1];
    *b = ansi_palette[index][2];
}

// Render one pane's visible ANSI rows with per-cell colours.
//
// Each row is walked three times, grouping adjacent cells that share an
// attribute into a single run so one glDrawArrays covers a whole colour span:
//   1. background quads for runs whose bg is not the default,
//   2. text runs sharing an (fg, bold) pair,
//   3. underline rules beneath runs with the underline attribute.
// Rows are laid out on the column grid (x0 + col * char_w) rather than by
// accumulating glyph advances, so the backgrounds line up with the glyphs.
static void render_ansi_pane(AnsiTerminal* term, Window* win, int start_row,
                             int max_rows, int max_cols, float x0, float y0,
                             float line_height) {
    if (!term || !win || !win->font) return;

    float font_size = win->font_size.value;
    float char_w = GetAverageCharWidth(win->font, font_size);
    float underline_h = font_size * 2.0f;
    if (underline_h < 1.0f) underline_h = 1.0f;

    if (max_cols > ANSI_BUFFER_COLS) max_cols = ANSI_BUFFER_COLS;

    AnsiCell cells[ANSI_BUFFER_COLS];
    char run[ANSI_BUFFER_COLS + 1];
    float y = y0;

    for (int row = 0; row < max_rows; row++, y += line_height) {
        int n = ansi_get_line_cells(term, start_row + row, cells, max_cols);
        if (n <= 0) continue;

        // The row occupies a full line_height band anchored on its text, so
        // adjacent rows' backgrounds tile without gaps or overlap.
        float row_top = y - line_height * 0.75f;

        // Pass 1: background runs.
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        for (int i = 0; i < n; ) {
            unsigned char bg = cells[i].bg_color;
            int j = i;
            while (j < n && cells[j].bg_color == bg) j++;

            if (bg != ANSI_DEFAULT_BG) {
                float r, g, b;
                ansi_color_rgb(bg, false, &r, &g, &b);
                glColor4f(r, g, b, 1.0f);
                float left  = x0 + i * char_w;
                float right = x0 + j * char_w;
                glVertex2f(left,  row_top);
                glVertex2f(right, row_top);
                glVertex2f(right, row_top + line_height);
                glVertex2f(left,  row_top + line_height);
            }
            i = j;
        }
        glEnd();

        // Pass 2: text runs sharing a foreground colour and weight.
        for (int i = 0; i < n; ) {
            unsigned char fg = cells[i].fg_color;
            bool bold = cells[i].bold;
            int j = i;
            while (j < n && cells[j].fg_color == fg && cells[j].bold == bold) j++;

            int len = 0;
            bool has_glyph = false;
            for (int k = i; k < j; k++) {
                run[len++] = cells[k].character;
                if (cells[k].character != ' ') has_glyph = true;
            }
            run[len] = '\0';

            if (has_glyph) {
                float r, g, b;
                ansi_color_rgb(fg, bold, &r, &g, &b);
                RenderTextColored(win->font, run, x0 + i * char_w, y,
                                  font_size, win->line_spacing.value, r, g, b, 1.0f);
            }
            i = j;
        }

        // Pass 3: underlines.
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        for (int i = 0; i < n; ) {
            unsigned char fg = cells[i].fg_color;
            bool bold = cells[i].bold;
            bool underline = cells[i].underline;
            int j = i;
            while (j < n && cells[j].underline == underline &&
                   cells[j].fg_color == fg && cells[j].bold == bold) j++;

            if (underline) {
                float r, g, b;
                ansi_color_rgb(fg, bold, &r, &g, &b);
                glColor4f(r, g, b, 1.0f);
                float left  = x0 + i * char_w;
                float right = x0 + j * char_w;
                float top   = y + font_size * 4.0f;
                glVertex2f(left,  top);
                glVertex2f(right, top);
                glVertex2f(right, top + underline_h);
                glVertex2f(left,  top + underline_h);
            }
            i = j;
        }
        glEnd();
        glDisable(GL_BLEND);
    }

    // Leave the pipeline in the white/untextured state the rest of the
    // renderer expects.
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// Render terminal content
void render_terminal_content() {
    if (!term_window || !term_window->font) return;


    ScreenCoord pos = wm_calculate_position(&wm, term_window);

    float line_height = term_window->font_size.value * 16.0f *
                        (1.0f + term_window->line_spacing.value);

    // Define the content area inside the window (respect 9-slice border)
    float border_padding = term_window->border_size.size;
    float content_left   = pos.x + term_window->text_margins.left + border_padding;
    float content_right  = pos.x + term_window->size.width  - term_window->text_margins.right - border_padding;
    float content_top    = pos.y + term_window->text_margins.top + border_padding;
    float content_bottom = pos.y + term_window->size.height - term_window->text_margins.bottom - border_padding;

    float x = content_left;
    // Add vertical padding for first line to prevent top clipping
    // Text baseline is at Y, but characters extend above it
    float y = content_top + (line_height * 0.75f);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Enable scissor test to clip text to content area
    // A pane squeezed past its chrome yields a negative rectangle, which
    // glScissor rejects outright - leaving the previous pane's rectangle in
    // force and clipping this one away entirely. Clamp instead so an
    // over-small pane simply draws nothing.
    int scissor_w = (int)(content_right - content_left);
    int scissor_h = (int)(content_bottom - content_top);
    if (scissor_w < 0) scissor_w = 0;
    if (scissor_h < 0) scissor_h = 0;

    glEnable(GL_SCISSOR_TEST);
    glScissor(
        (int)content_left,
        (int)(wm.screen_size.height - content_bottom),  // OpenGL scissor is from bottom
        scissor_w,
        scissor_h
    );

    if (interactive_mode) {
        float char_w = GetAverageCharWidth(term_window->font, term_window->font_size.value);
        int max_cols = terminal_cols_for_window(term_window);

        // How many rows fit vertically? Account for top padding we added
        float vertical_padding = line_height * 0.75f;
        int max_rows = (int)((content_bottom - content_top - vertical_padding) / line_height);
        // Subtract 1 row to ensure bottom text is never clipped (character descenders need room)
        if (max_rows > 1) max_rows -= 1;
        if (max_rows < 1) max_rows = 1;
        if (max_rows > ANSI_BUFFER_ROWS) max_rows = ANSI_BUFFER_ROWS;

        // Debug: Show what we're rendering (only print first time or when changed)
        static int last_max_rows = -1;
        static int last_max_cols = -1;
        if (max_rows != last_max_rows || max_cols != last_max_cols) {
            printf("Rendering %d rows x %d cols (content area: %.0f px, line height: %.1f, char width: %.1f)\n",
                   max_rows, max_cols, content_bottom - content_top, line_height, char_w);
            fflush(stdout);
            last_max_rows = max_rows;
            last_max_cols = max_cols;
        }

        // Scroll control: manual scroll or auto-follow cursor
        int cursor_x_check, cursor_y_check;
        ansi_get_cursor(&ansi_term, &cursor_x_check, &cursor_y_check);

        int start_row = 0;
        if (ansi_is_alt_screen(&ansi_term)) {
            // The alternate screen has no scrollback: it is exactly the
            // visible screen, so it is always drawn from row 0.
            start_row = 0;
        } else if (ansi_scroll_offset == 0) {
            // Auto-scroll to keep cursor visible (default behavior)
            // PTY has max_rows-2 rows, so cursor maxes at max_rows-3 (0-indexed)
            if (cursor_y_check > max_rows - 4) {
                start_row = cursor_y_check - max_rows + 4;
            }
            if (start_row < 0) start_row = 0;
        } else {
            // Manual scroll mode: use ansi_scroll_offset
            start_row = ansi_scroll_offset;
            if (start_row < 0) start_row = 0;
            if (start_row > ANSI_BUFFER_ROWS - max_rows) {
                start_row = ANSI_BUFFER_ROWS - max_rows;
            }
        }

        render_ansi_pane(&ansi_term, term_window, start_row, max_rows, max_cols,
                         x, y, line_height);

        glDisable(GL_SCISSOR_TEST);

        // Mark terminal as clean after rendering
        ansi_mark_clean(&ansi_term);

        // Render focus indicator in bottom left corner (after scissor test is disabled)
        // Show in split mode when left is focused, or in single terminal mode
        if ((split_horizontal && focused_terminal == 0) || !split_horizontal) {
            // Draw white box in bottom left corner of focused terminal
            float box_size = 20.0f;
            float box_x = pos.x + 20.0f;
            float box_y = pos.y + term_window->size.height - box_size - 20.0f;

            glDisable(GL_TEXTURE_2D);
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
            glBegin(GL_QUADS);
            glVertex2f(box_x, box_y);
            glVertex2f(box_x + box_size, box_y);
            glVertex2f(box_x + box_size, box_y + box_size);
            glVertex2f(box_x, box_y + box_size);
            glEnd();
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }

        return;
    }

    // Non-interactive mode - scrollback rendering
    // Calculate max_cols for input prompt truncation
    int max_cols = terminal_cols_for_window(term_window);

    float available_height = content_bottom - content_top - line_height; // Reserve space for input prompt (1 line)
    int max_visible_lines = (int)(available_height / line_height);
    if (max_visible_lines < 1) max_visible_lines = 1;

    int start_line = terminal.scroll_offset;
    int end_line = (terminal.line_count < start_line + max_visible_lines) ?
                    terminal.line_count : start_line + max_visible_lines;

    for (int i = start_line; i < end_line; i++) {
        if (i >= 0 && i < terminal.line_count) {
            // Clamp line to max_cols to prevent overflow past border
            char line_buf[MAX_LINE_LENGTH];
            strncpy(line_buf, terminal.lines[i], max_cols);
            line_buf[max_cols] = '\0';

            RenderText(term_window->font, line_buf, x, y,
                      term_window->font_size.value, term_window->line_spacing.value);
            y += line_height;
        }
    }

    // Render input prompt with cursor at correct position
    // Support multi-line wrapping when prompt + input exceeds max_cols
    char prompt_line[MAX_LINE_LENGTH * 2];

    // Build the full prompt line with cursor
    if (terminal.cursor_visible && terminal.cursor_pos < (int)strlen(terminal.input_buffer)) {
        // Cursor is in the middle - insert it at cursor_pos
        snprintf(prompt_line, sizeof(prompt_line), "%s", terminal.prompt);
        strncat(prompt_line, terminal.input_buffer, terminal.cursor_pos);
        strncat(prompt_line, "_", sizeof(prompt_line) - strlen(prompt_line) - 1);
        strncat(prompt_line, terminal.input_buffer + terminal.cursor_pos,
                sizeof(prompt_line) - strlen(prompt_line) - 1);
    } else {
        // Cursor at end
        snprintf(prompt_line, sizeof(prompt_line), "%s%s", terminal.prompt, terminal.input_buffer);
        if (terminal.cursor_visible) {
            strncat(prompt_line, "_", sizeof(prompt_line) - strlen(prompt_line) - 1);
        }
    }

    // Wrap the prompt line if it's too long
    int prompt_len = strlen(prompt_line);
    if (prompt_len <= max_cols) {
        // Single line - render normally
        RenderText(term_window->font, prompt_line, x, y,
                  term_window->font_size.value, term_window->line_spacing.value);
    } else {
        // Multi-line - wrap and render
        int pos = 0;
        while (pos < prompt_len) {
            char line_segment[MAX_LINE_LENGTH];
            int chars_to_render = (prompt_len - pos < max_cols) ? prompt_len - pos : max_cols;

            strncpy(line_segment, prompt_line + pos, chars_to_render);
            line_segment[chars_to_render] = '\0';

            RenderText(term_window->font, line_segment, x, y,
                      term_window->font_size.value, term_window->line_spacing.value);

            y += line_height;
            pos += chars_to_render;
        }
    }

    glDisable(GL_SCISSOR_TEST);
}

// Render right terminal content (for split mode)
void render_terminal_content_right() {
    if (!term_window_right || !term_window_right->font) return;


    ScreenCoord pos = wm_calculate_position(&wm, term_window_right);

    float line_height = term_window_right->font_size.value * 16.0f *
                        (1.0f + term_window_right->line_spacing.value);

    // Define the content area inside the window (respect 9-slice border)
    float border_padding = term_window_right->border_size.size;
    float content_left   = pos.x + term_window_right->text_margins.left + border_padding;
    float content_right  = pos.x + term_window_right->size.width  - term_window_right->text_margins.right - border_padding;
    float content_top    = pos.y + term_window_right->text_margins.top + border_padding;
    float content_bottom = pos.y + term_window_right->size.height - term_window_right->text_margins.bottom - border_padding;

    float x = content_left;
    float y = content_top + (line_height * 0.75f);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Enable scissor test to clip text to content area
    // A pane squeezed past its chrome yields a negative rectangle, which
    // glScissor rejects outright - leaving the previous pane's rectangle in
    // force and clipping this one away entirely. Clamp instead so an
    // over-small pane simply draws nothing.
    int scissor_w = (int)(content_right - content_left);
    int scissor_h = (int)(content_bottom - content_top);
    if (scissor_w < 0) scissor_w = 0;
    if (scissor_h < 0) scissor_h = 0;

    glEnable(GL_SCISSOR_TEST);
    glScissor(
        (int)content_left,
        (int)(wm.screen_size.height - content_bottom),  // OpenGL scissor is from bottom
        scissor_w,
        scissor_h
    );

    if (interactive_mode_right) {
        int max_cols = terminal_cols_for_window(term_window_right);

        // Calculate rows
        float vertical_padding = line_height * 0.75f;
        int max_rows = (int)((content_bottom - content_top - vertical_padding) / line_height);
        // Subtract 1 row to ensure bottom text is never clipped (character descenders need room)
        if (max_rows > 1) max_rows -= 1;
        if (max_rows < 1) max_rows = 1;
        if (max_rows > ANSI_BUFFER_ROWS) max_rows = ANSI_BUFFER_ROWS;

        // Scroll control: manual scroll or auto-follow cursor
        int cursor_x_check, cursor_y_check;
        ansi_get_cursor(&ansi_term_right, &cursor_x_check, &cursor_y_check);

        int start_row = 0;
        if (ansi_is_alt_screen(&ansi_term_right)) {
            // The alternate screen has no scrollback: it is exactly the
            // visible screen, so it is always drawn from row 0.
            start_row = 0;
        } else if (ansi_scroll_offset_right == 0) {
            // Auto-scroll to keep cursor visible (default behavior)
            // PTY has max_rows-2 rows, so cursor maxes at max_rows-3 (0-indexed)
            if (cursor_y_check > max_rows - 4) {
                start_row = cursor_y_check - max_rows + 4;
            }
            if (start_row < 0) start_row = 0;
        } else {
            // Manual scroll mode: use ansi_scroll_offset_right
            start_row = ansi_scroll_offset_right;
            if (start_row < 0) start_row = 0;
            if (start_row > ANSI_BUFFER_ROWS - max_rows) {
                start_row = ANSI_BUFFER_ROWS - max_rows;
            }
        }

        render_ansi_pane(&ansi_term_right, term_window_right, start_row, max_rows, max_cols,
                         x, y, line_height);

        glDisable(GL_SCISSOR_TEST);

        // Mark terminal as clean after rendering
        ansi_mark_clean(&ansi_term_right);

        // Render focus indicator in bottom left corner of right terminal (after scissor test is disabled)
        if (focused_terminal == 1) {
            // Draw white box in bottom left corner of focused terminal
            float box_size = 20.0f;
            float box_x = pos.x + 20.0f;
            float box_y = pos.y + term_window_right->size.height - box_size - 20.0f;

            glDisable(GL_TEXTURE_2D);
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
            glBegin(GL_QUADS);
            glVertex2f(box_x, box_y);
            glVertex2f(box_x + box_size, box_y);
            glVertex2f(box_x + box_size, box_y + box_size);
            glVertex2f(box_x, box_y + box_size);
            glEnd();
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }

        return;
    }

    // Non-interactive mode - scrollback rendering
    // Calculate max_cols for input prompt truncation
    int max_cols = terminal_cols_for_window(term_window_right);

    float available_height = content_bottom - content_top - line_height;
    int max_visible_lines = (int)(available_height / line_height);
    if (max_visible_lines < 1) max_visible_lines = 1;

    int start_line = terminal_right.scroll_offset;
    int end_line = (terminal_right.line_count < start_line + max_visible_lines) ?
                    terminal_right.line_count : start_line + max_visible_lines;

    for (int i = start_line; i < end_line; i++) {
        if (i >= 0 && i < terminal_right.line_count) {
            RenderText(term_window_right->font, terminal_right.lines[i], x, y,
                      term_window_right->font_size.value, term_window_right->line_spacing.value);
            y += line_height;
        }
    }

    // Render input prompt with cursor at correct position
    // Support multi-line wrapping when prompt + input exceeds max_cols
    char prompt_line[MAX_LINE_LENGTH * 2];

    // Build the full prompt line with cursor
    if (terminal_right.cursor_visible && terminal_right.cursor_pos < (int)strlen(terminal_right.input_buffer)) {
        snprintf(prompt_line, sizeof(prompt_line), "%s", terminal_right.prompt);
        strncat(prompt_line, terminal_right.input_buffer, terminal_right.cursor_pos);
        strncat(prompt_line, "_", sizeof(prompt_line) - strlen(prompt_line) - 1);
        strncat(prompt_line, terminal_right.input_buffer + terminal_right.cursor_pos,
                sizeof(prompt_line) - strlen(prompt_line) - 1);
    } else {
        snprintf(prompt_line, sizeof(prompt_line), "%s%s", terminal_right.prompt, terminal_right.input_buffer);
        if (terminal_right.cursor_visible) {
            strncat(prompt_line, "_", sizeof(prompt_line) - strlen(prompt_line) - 1);
        }
    }

    // Wrap the prompt line if it's too long
    int prompt_len = strlen(prompt_line);
    if (prompt_len <= max_cols) {
        // Single line - render normally
        RenderText(term_window_right->font, prompt_line, x, y,
                  term_window_right->font_size.value, term_window_right->line_spacing.value);
    } else {
        // Multi-line - wrap and render
        int pos = 0;
        while (pos < prompt_len) {
            char line_segment[MAX_LINE_LENGTH];
            int chars_to_render = (prompt_len - pos < max_cols) ? prompt_len - pos : max_cols;

            strncpy(line_segment, prompt_line + pos, chars_to_render);
            line_segment[chars_to_render] = '\0';

            RenderText(term_window_right->font, line_segment, x, y,
                      term_window_right->font_size.value, term_window_right->line_spacing.value);

            y += line_height;
            pos += chars_to_render;
        }
    }

    glDisable(GL_SCISSOR_TEST);
}

// Render bottom terminal content (for split mode)
void render_terminal_content_bottom() {
    if (!term_window_bottom || !term_window_bottom->font) return;


    ScreenCoord pos = wm_calculate_position(&wm, term_window_bottom);

    float line_height = term_window_bottom->font_size.value * 16.0f *
                        (1.0f + term_window_bottom->line_spacing.value);

    // Define the content area inside the window (respect 9-slice border)
    float border_padding = term_window_bottom->border_size.size;
    float content_left   = pos.x + term_window_bottom->text_margins.left + border_padding;
    float content_right  = pos.x + term_window_bottom->size.width  - term_window_bottom->text_margins.right - border_padding;
    float content_top    = pos.y + term_window_bottom->text_margins.top + border_padding;
    float content_bottom = pos.y + term_window_bottom->size.height - term_window_bottom->text_margins.bottom - border_padding;

    float x = content_left;
    float y = content_top + (line_height * 0.75f);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Enable scissor test to clip text to content area
    // A pane squeezed past its chrome yields a negative rectangle, which
    // glScissor rejects outright - leaving the previous pane's rectangle in
    // force and clipping this one away entirely. Clamp instead so an
    // over-small pane simply draws nothing.
    int scissor_w = (int)(content_right - content_left);
    int scissor_h = (int)(content_bottom - content_top);
    if (scissor_w < 0) scissor_w = 0;
    if (scissor_h < 0) scissor_h = 0;

    glEnable(GL_SCISSOR_TEST);
    glScissor(
        (int)content_left,
        (int)(wm.screen_size.height - content_bottom),  // OpenGL scissor is from bottom
        scissor_w,
        scissor_h
    );

    // Interactive mode
    if (interactive_mode_bottom) {
        int max_cols = terminal_cols_for_window(term_window_bottom);

        float vertical_padding = line_height * 0.75f;
        int max_rows = (int)((content_bottom - content_top - vertical_padding) / line_height);
        // Subtract 1 row to ensure bottom text is never clipped (character descenders need room)
        if (max_rows > 1) max_rows -= 1;
        if (max_rows < 1) max_rows = 1;
        if (max_rows > ANSI_BUFFER_ROWS) max_rows = ANSI_BUFFER_ROWS;

        // Scroll control: manual scroll or auto-follow cursor
        int cursor_x_check, cursor_y_check;
        ansi_get_cursor(&ansi_term_bottom, &cursor_x_check, &cursor_y_check);

        int start_row = 0;
        if (ansi_is_alt_screen(&ansi_term_bottom)) {
            // The alternate screen has no scrollback: it is exactly the
            // visible screen, so it is always drawn from row 0.
            start_row = 0;
        } else if (ansi_scroll_offset_bottom == 0) {
            // Auto-scroll to keep cursor visible (default behavior)
            if (cursor_y_check > max_rows - 4) {
                start_row = cursor_y_check - max_rows + 4;
            }
            if (start_row < 0) start_row = 0;
        } else {
            // Manual scroll mode: use ansi_scroll_offset_bottom
            start_row = ansi_scroll_offset_bottom;
            if (start_row < 0) start_row = 0;
            if (start_row > ANSI_BUFFER_ROWS - max_rows) {
                start_row = ANSI_BUFFER_ROWS - max_rows;
            }
        }

        render_ansi_pane(&ansi_term_bottom, term_window_bottom, start_row, max_rows, max_cols,
                         x, y, line_height);

        glDisable(GL_SCISSOR_TEST);

        // Mark terminal as clean after rendering
        ansi_mark_clean(&ansi_term_bottom);

        // Render focus indicator
        if (focused_terminal == 1) {
            float box_size = 20.0f;
            float box_x = pos.x + 20.0f;
            float box_y = pos.y + term_window_bottom->size.height - box_size - 20.0f;

            glDisable(GL_TEXTURE_2D);
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
            glBegin(GL_QUADS);
            glVertex2f(box_x, box_y);
            glVertex2f(box_x + box_size, box_y);
            glVertex2f(box_x + box_size, box_y + box_size);
            glVertex2f(box_x, box_y + box_size);
            glEnd();
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

// Mouse position callback
void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    context_menu.mouse_x = xpos;
    context_menu.mouse_y = ypos;

    if (context_menu.visible) {
        float menu_height = MENU_ITEM_COUNT * MENU_ITEM_HEIGHT;

        context_menu.selected_item = -1;

        if (xpos >= context_menu.x && xpos <= context_menu.x + MENU_WIDTH &&
            ypos >= context_menu.y && ypos <= context_menu.y + menu_height) {

            float relative_y = ypos - context_menu.y;   // 0 at top
            int index = (int)(relative_y / MENU_ITEM_HEIGHT);

            if (index >= 0 && index < MENU_ITEM_COUNT)
                context_menu.selected_item = index;     // 0 = top, 5 = bottom
        }
    }
}

// Mouse scroll callback
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;

    // Determine which terminal window the mouse is over
    double mouse_x = context_menu.mouse_x;
    double mouse_y = context_menu.mouse_y;

    // Check which terminal the mouse is over and scroll it
    TerminalState* target_term = NULL;
    Window* target_window = NULL;

    // Helper to check if point is inside window bounds
    if (split_horizontal) {
        // Horizontal split: check right terminal first, then left
        if (term_window_right) {
            ScreenCoord pos = wm_calculate_position(&wm, term_window_right);
            if (mouse_x >= pos.x && mouse_x <= pos.x + term_window_right->size.width &&
                mouse_y >= pos.y && mouse_y <= pos.y + term_window_right->size.height) {
                target_term = &terminal_right;
                target_window = term_window_right;
            }
        }
        if (!target_term && term_window) {
            ScreenCoord pos = wm_calculate_position(&wm, term_window);
            if (mouse_x >= pos.x && mouse_x <= pos.x + term_window->size.width &&
                mouse_y >= pos.y && mouse_y <= pos.y + term_window->size.height) {
                target_term = &terminal;
                target_window = term_window;
            }
        }
    } else if (split_vertical) {
        // Vertical split: check bottom terminal first, then top
        if (term_window_bottom) {
            ScreenCoord pos = wm_calculate_position(&wm, term_window_bottom);
            if (mouse_x >= pos.x && mouse_x <= pos.x + term_window_bottom->size.width &&
                mouse_y >= pos.y && mouse_y <= pos.y + term_window_bottom->size.height) {
                target_term = &terminal_bottom;
                target_window = term_window_bottom;
            }
        }
        if (!target_term && term_window) {
            ScreenCoord pos = wm_calculate_position(&wm, term_window);
            if (mouse_x >= pos.x && mouse_x <= pos.x + term_window->size.width &&
                mouse_y >= pos.y && mouse_y <= pos.y + term_window->size.height) {
                target_term = &terminal;
                target_window = term_window;
            }
        }
    } else {
        // Single terminal mode
        if (term_window) {
            ScreenCoord pos = wm_calculate_position(&wm, term_window);
            if (mouse_x >= pos.x && mouse_x <= pos.x + term_window->size.width &&
                mouse_y >= pos.y && mouse_y <= pos.y + term_window->size.height) {
                target_term = &terminal;
                target_window = term_window;
            }
        }
    }

    if (target_term && target_window && target_window->font) {
        // Determine if this terminal is in interactive mode
        bool is_interactive = false;
        int* ansi_scroll_ptr = NULL;

        if (split_horizontal && target_term == &terminal_right && interactive_mode_right) {
            is_interactive = true;
            ansi_scroll_ptr = &ansi_scroll_offset_right;
        } else if (split_vertical && target_term == &terminal_bottom && interactive_mode_bottom) {
            is_interactive = true;
            ansi_scroll_ptr = &ansi_scroll_offset_bottom;
        } else if (target_term == &terminal && interactive_mode) {
            is_interactive = true;
            ansi_scroll_ptr = &ansi_scroll_offset;
        }

        if (is_interactive && ansi_scroll_ptr) {
            // Interactive mode: scroll the ANSI buffer
            int cursor_x, cursor_y;
            AnsiTerminal* ansi_term_ptr = NULL;

            if (split_horizontal && target_term == &terminal_right) {
                ansi_term_ptr = &ansi_term_right;
            } else if (split_vertical && target_term == &terminal_bottom) {
                ansi_term_ptr = &ansi_term_bottom;
            } else if (target_term == &terminal) {
                ansi_term_ptr = &ansi_term;
            }

            if (ansi_term_ptr) {
                ansi_get_cursor(ansi_term_ptr, &cursor_x, &cursor_y);

                float line_height = target_window->font_size.value * 16.0f *
                                   (1.0f + target_window->line_spacing.value);
                float available_height = target_window->size.height -
                                        target_window->text_margins.top -
                                        target_window->text_margins.bottom;
                int max_visible_lines = (int)(available_height / line_height);
                if (max_visible_lines < 1) max_visible_lines = 1;

                // Calculate what auto-follow position would be
                int auto_follow_row = 0;
                if (cursor_y > max_visible_lines - 4) {
                    auto_follow_row = cursor_y - max_visible_lines + 4;
                }
                if (auto_follow_row < 0) auto_follow_row = 0;

                // Calculate current start_row (what we're actually displaying)
                int current_start_row;
                if (*ansi_scroll_ptr == 0) {
                    // In auto-follow mode
                    current_start_row = auto_follow_row;
                } else {
                    // In manual scroll mode
                    current_start_row = *ansi_scroll_ptr;
                }

                // Apply scroll: UP decreases start_row (older), DOWN increases (newer)
                // yoffset > 0 = scroll UP = view older = decrease start_row
                // yoffset < 0 = scroll DOWN = view newer = increase start_row
                int scroll_amount = (int)(yoffset * 3.0);
                int new_start_row = current_start_row - scroll_amount;

                // The ANSI buffer is a ring buffer - be very conservative to avoid wrap-around
                // Only allow scrolling back through content that's definitely still valid
                int min_scroll = 0;

                // Conservative limit: don't scroll back more than (buffer_size - max_visible - margin)
                // This ensures we never hit wrapped/overwritten content
                int safe_scrollback = ANSI_BUFFER_ROWS - max_visible_lines - 50;
                if (safe_scrollback < 0) safe_scrollback = 0;

                if (cursor_y > safe_scrollback) {
                    min_scroll = cursor_y - safe_scrollback;
                }

                // Maximum scroll position: don't scroll past the cursor
                int max_scroll = cursor_y;
                if (max_scroll < 0) max_scroll = 0;

                // Also ensure we don't exceed buffer boundaries
                int buffer_max = ANSI_BUFFER_ROWS - max_visible_lines;
                if (buffer_max < 0) buffer_max = 0;
                if (max_scroll > buffer_max) max_scroll = buffer_max;

                // Clamp to valid range [min_scroll, max_scroll]
                if (new_start_row < min_scroll) new_start_row = min_scroll;
                if (new_start_row > max_scroll) new_start_row = max_scroll;

                // If scrolled to or past auto-follow position, return to auto-follow mode
                if (new_start_row >= auto_follow_row) {
                    *ansi_scroll_ptr = 0;  // Return to auto-follow
                } else {
                    *ansi_scroll_ptr = new_start_row;
                }
            }
        } else {
            // Non-interactive mode: use terminal scrollback
            // yoffset > 0 = scroll UP (view older) = DECREASE offset
            // yoffset < 0 = scroll DOWN (view newer) = INCREASE offset
            int scroll_delta = -(int)(yoffset * 3.0);

            float line_height = target_window->font_size.value * 16.0f *
                               (1.0f + target_window->line_spacing.value);
            float available_height = target_window->size.height -
                                    target_window->text_margins.top -
                                    target_window->text_margins.bottom;
            int max_visible_lines = (int)(available_height / line_height);
            if (max_visible_lines < 1) max_visible_lines = 1;

            int max_scroll = target_term->line_count - max_visible_lines + 1;
            if (max_scroll < 0) max_scroll = 0;

            // Apply scroll delta
            target_term->scroll_offset += scroll_delta;

            // Clamp to valid range
            if (target_term->scroll_offset < 0) {
                target_term->scroll_offset = 0;
            }
            if (target_term->scroll_offset > max_scroll) {
                target_term->scroll_offset = max_scroll;
            }
        }
    }
}

// Mouse button callback
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;

    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        context_menu.visible = true;
        context_menu.x = context_menu.mouse_x;
        int window_width, window_height;
        glfwGetWindowSize(window, &window_width, &window_height);

        float menu_height = MENU_ITEM_COUNT * MENU_ITEM_HEIGHT;

        // Keep everything in top-left coords
        context_menu.y = context_menu.mouse_y;

        // Clamp to screen
        if (context_menu.y + menu_height > window_height)
            context_menu.y = window_height - menu_height;
        if (context_menu.y < 0) context_menu.y = 0;

        if (context_menu.x + MENU_WIDTH > window_width)
            context_menu.x = window_width - MENU_WIDTH;
        if (context_menu.x < 0) context_menu.x = 0;

        context_menu.selected_item = -1;
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        // Check for focus switching in split mode
        if (split_horizontal && term_window && term_window_right) {
            double mouse_x = context_menu.mouse_x;
            double mouse_y = context_menu.mouse_y;

            // Get window bounds for both terminals
            ScreenCoord left_pos = wm_calculate_position(&wm, term_window);
            ScreenCoord right_pos = wm_calculate_position(&wm, term_window_right);

            bool clicked_left = (mouse_x >= left_pos.x &&
                                mouse_x <= left_pos.x + term_window->size.width &&
                                mouse_y >= left_pos.y &&
                                mouse_y <= left_pos.y + term_window->size.height);

            bool clicked_right = (mouse_x >= right_pos.x &&
                                 mouse_x <= right_pos.x + term_window_right->size.width &&
                                 mouse_y >= right_pos.y &&
                                 mouse_y <= right_pos.y + term_window_right->size.height);

            if (clicked_left && focused_terminal != 0) {
                focused_terminal = 0;
                printf("Focus switched to LEFT terminal\n");
                fflush(stdout);
            } else if (clicked_right && focused_terminal != 1) {
                focused_terminal = 1;
                printf("Focus switched to RIGHT terminal\n");
                fflush(stdout);
            }
        } else if (split_vertical && term_window && term_window_bottom) {
            double mouse_x = context_menu.mouse_x;
            double mouse_y = context_menu.mouse_y;

            // Get window bounds for both terminals
            ScreenCoord top_pos = wm_calculate_position(&wm, term_window);
            ScreenCoord bottom_pos = wm_calculate_position(&wm, term_window_bottom);

            bool clicked_top = (mouse_x >= top_pos.x &&
                               mouse_x <= top_pos.x + term_window->size.width &&
                               mouse_y >= top_pos.y &&
                               mouse_y <= top_pos.y + term_window->size.height);

            bool clicked_bottom = (mouse_x >= bottom_pos.x &&
                                  mouse_x <= bottom_pos.x + term_window_bottom->size.width &&
                                  mouse_y >= bottom_pos.y &&
                                  mouse_y <= bottom_pos.y + term_window_bottom->size.height);

            if (clicked_top && focused_terminal != 0) {
                focused_terminal = 0;
                printf("Focus switched to TOP terminal\n");
                fflush(stdout);
            } else if (clicked_bottom && focused_terminal != 1) {
                focused_terminal = 1;
                printf("Focus switched to BOTTOM terminal\n");
                fflush(stdout);
            }
        }

        if (context_menu.visible) {
            if (context_menu.selected_item >= 0) {
                // Handle menu item click
                switch (context_menu.selected_item) {
                    case 0:  // Copy
                        // TODO: Implement clipboard copy
                        printf("Copy selected\n");
                        break;
                    case 1:  // Paste
                        {
                            const char* clipboard_text = glfwGetClipboardString(window);
                            if (clipboard_text && clipboard_text[0] != '\0') {
                                if (interactive_mode && pty_master_fd >= 0) {
                                    // In PTY mode, write clipboard to PTY
                                    ssize_t bytes_written = write(pty_master_fd, clipboard_text, strlen(clipboard_text));
                                    if (bytes_written < 0) {
                                        perror("Failed to paste to PTY");
                                    } else {
                                        printf("Pasted %zd bytes to PTY\n", bytes_written);
                                    }
                                } else {
                                    // In non-interactive mode, insert at cursor
                                    size_t clipboard_len = strlen(clipboard_text);
                                    size_t current_len = strlen(terminal.input_buffer);

                                    // Validate cursor position
                                    if (terminal.cursor_pos < 0 || (size_t)terminal.cursor_pos > current_len) {
                                        terminal.cursor_pos = current_len;
                                    }

                                    // Check if there's room (need space for text + null terminator)
                                    if (current_len + clipboard_len < MAX_LINE_LENGTH) {
                                        // Make room for pasted text
                                        memmove(terminal.input_buffer + terminal.cursor_pos + clipboard_len,
                                                terminal.input_buffer + terminal.cursor_pos,
                                                current_len - terminal.cursor_pos + 1);

                                        // Insert clipboard text
                                        memcpy(terminal.input_buffer + terminal.cursor_pos,
                                               clipboard_text, clipboard_len);

                                        terminal.cursor_pos += clipboard_len;

                                        // Ensure null termination (defensive)
                                        terminal.input_buffer[current_len + clipboard_len] = '\0';

                                        printf("Pasted %zu bytes to input buffer\n", clipboard_len);
                                    } else {
                                        printf("Clipboard text too long to paste (max %d chars)\n",
                                               MAX_LINE_LENGTH - 1);
                                    }
                                }
                            } else {
                                printf("Clipboard is empty\n");
                            }
                        }
                        break;
                    case 2:  // Clear Screen
                        if (interactive_mode) {
                            ansi_clear(&ansi_term);
                        } else {
                            terminal.line_count = 0;
                            terminal.scroll_offset = 0;
                        }
                        break;
                    case 3:  // Font Size +
                        term_window->font_size.value += 0.1f;
                        if (term_window->font_size.value > 5.0f) term_window->font_size.value = 5.0f;
                        printf("Font size: %.1f (%.0fpx)\n", term_window->font_size.value,
                               term_window->font_size.value * 16.0f);
                        resize_pty_to_window();
                        rewrap_terminal_content();
                        break;
                    case 4:  // Font Size -
                        term_window->font_size.value -= 0.1f;
                        if (term_window->font_size.value < 0.5f) term_window->font_size.value = 0.5f;
                        printf("Font size: %.1f (%.0fpx)\n", term_window->font_size.value,
                               term_window->font_size.value * 16.0f);
                        resize_pty_to_window();
                        rewrap_terminal_content();
                        break;
                    case 5:  // Split Vertical
                        printf("Split Vertical selected\n");
                        if (split_horizontal) {
                            // Already split vertically (left/right) -> UNSPLIT
                            printf("Unsplitting vertical split...\n");

                            // Close right terminal PTY
                            if (pty_master_fd_right >= 0) {
                                close(pty_master_fd_right);
                                pty_master_fd_right = -1;
                            }

                            // Kill right terminal child process
                            if (pty_child_pid_right > 0) {
                                kill(pty_child_pid_right, SIGTERM);
                                waitpid(pty_child_pid_right, NULL, WNOHANG);
                                pty_child_pid_right = -1;
                            }

                            // Free right terminal font
                            if (term_window_right && term_window_right->font) {
                                FreeFont(term_window_right->font);
                                term_window_right->font = NULL;
                            }

                            // Clear right terminal buffer
                            terminal_right.line_count = 0;
                            terminal_right.raw_line_count = 0;
                            terminal_right.scroll_offset = 0;
                            terminal_right.cursor_pos = 0;
                            terminal_right.input_buffer[0] = '\0';
                            terminal_right.history_count = 0;
                            terminal_right.history_index = -1;
                            ansi_clear(&ansi_term_right);
                            ansi_scroll_offset_right = 0;

                            // Hide right window
                            if (term_window_right) {
                                wm_hide_window(&wm, term_window_right->name);
                                term_window_right = NULL;
                            }

                            // Reset split flag and focus
                            split_horizontal = false;
                            interactive_mode_right = false;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Restore main terminal to full size
                            term_window->size = WINDOW_SIZE(current_width - 40, current_height - 40);
                            term_window->anchor = ANCHOR_CENTER;
                            term_window->position = SCREEN_COORD(0, 0);

                            // CRITICAL: Recreate 9-slice for restored full-size terminal
                            NineSliceParams restore_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &restore_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap main terminal content for new width
                            rewrap_terminal_content();

                            // Resize PTY
                            resize_pty_to_window();

                            printf("Vertical split disabled - returned to single terminal\n");
                        } else if (split_vertical) {
                            printf("Cannot split: terminal is already split horizontally\n");
                        } else {
                            // Not split -> Split vertically (left/right)
                            split_horizontal = true;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Calculate usable area and split sizes
                            int usable_width = current_width - (WINDOW_EDGE_PADDING * 2);
                            int usable_height = current_height - (WINDOW_EDGE_PADDING * 2);
                            int half_width = (usable_width - TERMINAL_GAP) / 2;

                            // Resize left window
                            term_window->size = WINDOW_SIZE(half_width, usable_height);
                            term_window->anchor = ANCHOR_CENTER_LEFT;
                            term_window->position = SCREEN_COORD(WINDOW_EDGE_PADDING, 0);
                            // Keep existing margins (don't reset user adjustments)
                            term_window->center_alpha = 0.9f;

                            // CRITICAL: Recreate 9-slice for left terminal with new size
                            NineSliceParams left_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &left_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap left terminal content for new width
                            rewrap_terminal_content();

                            // CRITICAL: Resize PTY for left terminal (it was just resized to half width)
                            resize_pty_to_window();

                            // Create right terminal window
                            // Reuse this pane's window if an earlier split already
                            // created it. The window manager is a bump allocator with
                            // no free list, so making a fresh one on every split walks
                            // window_count toward MAX_WINDOWS and then hands back an
                            // invalid id - which this code indexed regardless, writing
                            // outside the array.
                            term_window_right = wm_get_window(&wm, window_name_from_string("terminal_right"));
                            if (!term_window_right) {
                                WindowID right_id = wm_create_window(&wm, window_name_from_string("terminal_right"));
                                term_window_right = window_id_is_valid(right_id)
                                               ? &wm.windows[right_id.value] : NULL;
                            }
                            if (!term_window_right) {
                                fprintf(stderr, "Cannot split: no window slots available\n");
                                split_horizontal = false;
                                int restore_w = 0, restore_h = 0;
                                glfwGetWindowSize(window, &restore_w, &restore_h);
                                term_window->size = WINDOW_SIZE(restore_w - 40, restore_h - 40);
                                term_window->anchor = ANCHOR_CENTER;
                                term_window->position = SCREEN_COORD(0, 0);
                                resize_pty_to_window();
                                break;
                            }

                            // Configure right window - FIXED: Copy layout from left terminal
                            term_window_right->anchor = ANCHOR_CENTER_RIGHT;
                            term_window_right->position = SCREEN_COORD(-WINDOW_EDGE_PADDING, 0);
                            term_window_right->size = WINDOW_SIZE(half_width, usable_height);
                            term_window_right->texture_path = font_path_from_string(asset_path("assets/ui/9-slice-basice9.png"));
                            term_window_right->text_color = TEXT_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
                            term_window_right->font_size = FONT_SIZE(1.0f);
                            // FIXED: Copy border size from left terminal instead of hardcoding
                            term_window_right->border_size = term_window->border_size;
                            term_window_right->line_spacing = LINE_SPACING(0.30f);
                            // FIXED: Copy margins from left terminal for identical padding
                            term_window_right->text_margins = term_window->text_margins;
                            term_window_right->center_alpha = 0.9f;

                            // CRITICAL: separate font instance per pane to prevent a
                            // double free. Only load it the first time this pane's
                            // window is set up, or reusing the window leaks one font
                            // per split.
                            if (!term_window_right->font) {
                                term_window_right->font = LoadFont(asset_path("assets/fonts/font_basis33.json"),
                                                    asset_path("assets/fonts/font_basis33.png"));
                                if (!term_window_right->font) {
                                    fprintf(stderr, "Failed to load font for right terminal\n");
                                }
                            }

                            // Initialize right terminal
                            ansi_init(&ansi_term_right);
                            terminal_right.line_count = 0;
                            terminal_right.scroll_offset = 0;
                            terminal_right.cursor_pos = 0;
                            terminal_right.input_buffer[0] = '\0';

                            // Start separate shell for right terminal
                            // Calculate actual window size
                            int rows = 24, cols = 80;
                            if (term_window_right && term_window_right->font) {
                                float char_h = term_window_right->font_size.value * 16.0f * (1.0f + term_window_right->line_spacing.value);
                                float border_padding = term_window_right->border_size.size * 2.0f;
                                float usable_h = term_window_right->size.height - term_window_right->text_margins.top -
                                                term_window_right->text_margins.bottom - border_padding;

                                // Account for vertical padding (matches rendering)
                                float vertical_padding_right = char_h * 0.75f;
                                usable_h -= vertical_padding_right;

                                cols = terminal_cols_for_window(term_window_right);
                                rows = (int)(usable_h / char_h);
                                if (rows > 3) rows -= 3;  // Match PTY resize: 2 for safety + 1 for descender clipping
                                if (rows < 10) rows = 10;
                                if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;
                            }

                            // Autowrap at exactly the width the child is told.
                            ansi_set_size(&ansi_term_right, cols, rows);
                            struct winsize ws = {
                                .ws_row = rows,
                                .ws_col = cols,
                                .ws_xpixel = 0,
                                .ws_ypixel = 0
                            };

                            pty_child_pid_right = forkpty(&pty_master_fd_right, NULL, NULL, &ws);
                            if (pty_child_pid_right == 0) {
                                // Child process - forkpty already created session and controlling terminal
                                // DO NOT call setsid()
                                for (int fd = 3; fd < 256; fd++) close(fd);
                                setenv("TERM", "xterm", 1);
                                setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);

                                // Configure terminal for SSH compatibility
                                struct termios tios;
                                if (tcgetattr(STDIN_FILENO, &tios) == 0) {
                                    tios.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
                                    tios.c_oflag |= OPOST | ONLCR;
                                    tios.c_iflag |= ICRNL | IXON;
                                    tcsetattr(STDIN_FILENO, TCSANOW, &tios);
                                }

                                exec_default_shell();
                                _exit(1);
                            } else {
                                // Parent: set non-blocking
                                int flags = fcntl(pty_master_fd_right, F_GETFL, 0);
                                if (flags >= 0) {
                                    fcntl(pty_master_fd_right, F_SETFL, flags | O_NONBLOCK);
                                }
                                interactive_mode_right = true;
                                pty_start_time_right = glfwGetTime();

                                // Set correct terminal size for the right PTY
                                resize_pty_to_window_right();
                            }

                            // CRITICAL: Create 9-slice for right terminal before showing
                            NineSliceParams right_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window_right->size,
                                .borderLeft = term_window_right->border_size,
                                .borderRight = term_window_right->border_size,
                                .borderTop = term_window_right->border_size,
                                .borderBottom = term_window_right->border_size
                            };
                            createNineSlice(&term_window_right->nine_slice, &right_params, WINDOW_SIZE(32, 32), term_window_right->texture_path.value);

                            wm_show_window(&wm, term_window_right->name);
                            printf("Vertical split enabled - two independent terminals with separate fonts\n");
                        }
                        break;
                    case 6:  // Split Horizontal
                        printf("Split Horizontal selected\n");
                        if (split_vertical) {
                            // Already split horizontally (top/bottom) -> UNSPLIT
                            printf("Unsplitting horizontal split...\n");

                            // Close bottom terminal PTY
                            if (pty_master_fd_bottom >= 0) {
                                close(pty_master_fd_bottom);
                                pty_master_fd_bottom = -1;
                            }

                            // Kill bottom terminal child process
                            if (pty_child_pid_bottom > 0) {
                                kill(pty_child_pid_bottom, SIGTERM);
                                waitpid(pty_child_pid_bottom, NULL, WNOHANG);
                                pty_child_pid_bottom = -1;
                            }

                            // Free bottom terminal font
                            if (term_window_bottom && term_window_bottom->font) {
                                FreeFont(term_window_bottom->font);
                                term_window_bottom->font = NULL;
                            }

                            // Clear bottom terminal buffer
                            terminal_bottom.line_count = 0;
                            terminal_bottom.raw_line_count = 0;
                            terminal_bottom.scroll_offset = 0;
                            terminal_bottom.cursor_pos = 0;
                            terminal_bottom.input_buffer[0] = '\0';
                            terminal_bottom.history_count = 0;
                            terminal_bottom.history_index = -1;
                            ansi_clear(&ansi_term_bottom);
                            ansi_scroll_offset_bottom = 0;

                            // Hide bottom window
                            if (term_window_bottom) {
                                wm_hide_window(&wm, term_window_bottom->name);
                                term_window_bottom = NULL;
                            }

                            // Reset split flag and focus
                            split_vertical = false;
                            interactive_mode_bottom = false;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Restore main terminal to full size
                            term_window->size = WINDOW_SIZE(current_width - 40, current_height - 40);
                            term_window->anchor = ANCHOR_CENTER;
                            term_window->position = SCREEN_COORD(0, 0);

                            // CRITICAL: Recreate 9-slice for restored full-size terminal
                            NineSliceParams restore_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &restore_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap main terminal content for new width
                            rewrap_terminal_content();

                            // Resize PTY
                            resize_pty_to_window();

                            printf("Horizontal split disabled - returned to single terminal\n");
                        } else if (split_horizontal) {
                            printf("Cannot split: terminal is already split vertically\n");
                        } else {
                            // Not split -> Split horizontally (top/bottom)
                            split_vertical = true;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Calculate usable area and split sizes
                            int usable_width = current_width - (WINDOW_EDGE_PADDING * 2);
                            int usable_height = current_height - (WINDOW_EDGE_PADDING * 2);
                            int half_height = (usable_height - TERMINAL_GAP) / 2;

                            // Resize top window
                            term_window->size = WINDOW_SIZE(usable_width, half_height);
                            term_window->anchor = ANCHOR_TOP_CENTER;
                            term_window->position = SCREEN_COORD(0, WINDOW_EDGE_PADDING);
                            // Keep existing margins (don't reset user adjustments)
                            term_window->center_alpha = 0.9f;

                            // CRITICAL: Recreate 9-slice for top terminal with new size
                            NineSliceParams top_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &top_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap top terminal content for new height
                            rewrap_terminal_content();

                            // CRITICAL: Resize PTY for top terminal (it was just resized to half height)
                            resize_pty_to_window();

                            // Create bottom terminal window
                            // Reuse this pane's window if an earlier split already
                            // created it. The window manager is a bump allocator with
                            // no free list, so making a fresh one on every split walks
                            // window_count toward MAX_WINDOWS and then hands back an
                            // invalid id - which this code indexed regardless, writing
                            // outside the array.
                            term_window_bottom = wm_get_window(&wm, window_name_from_string("terminal_bottom"));
                            if (!term_window_bottom) {
                                WindowID bottom_id = wm_create_window(&wm, window_name_from_string("terminal_bottom"));
                                term_window_bottom = window_id_is_valid(bottom_id)
                                               ? &wm.windows[bottom_id.value] : NULL;
                            }
                            if (!term_window_bottom) {
                                fprintf(stderr, "Cannot split: no window slots available\n");
                                split_vertical = false;
                                int restore_w = 0, restore_h = 0;
                                glfwGetWindowSize(window, &restore_w, &restore_h);
                                term_window->size = WINDOW_SIZE(restore_w - 40, restore_h - 40);
                                term_window->anchor = ANCHOR_CENTER;
                                term_window->position = SCREEN_COORD(0, 0);
                                resize_pty_to_window();
                                break;
                            }

                            // Configure bottom window - FIXED: Copy layout from top terminal
                            term_window_bottom->anchor = ANCHOR_BOTTOM_CENTER;
                            term_window_bottom->position = SCREEN_COORD(0, -WINDOW_EDGE_PADDING);
                            term_window_bottom->size = WINDOW_SIZE(usable_width, half_height);
                            term_window_bottom->texture_path = font_path_from_string(asset_path("assets/ui/9-slice-basice9.png"));
                            term_window_bottom->text_color = TEXT_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
                            term_window_bottom->font_size = FONT_SIZE(1.0f);
                            // FIXED: Copy border size from top terminal instead of hardcoding
                            term_window_bottom->border_size = term_window->border_size;
                            term_window_bottom->line_spacing = LINE_SPACING(0.30f);
                            // FIXED: Copy margins from top terminal for identical padding
                            term_window_bottom->text_margins = term_window->text_margins;
                            term_window_bottom->center_alpha = 0.9f;

                            // CRITICAL: separate font instance per pane to prevent a
                            // double free. Only load it the first time this pane's
                            // window is set up, or reusing the window leaks one font
                            // per split.
                            if (!term_window_bottom->font) {
                                term_window_bottom->font = LoadFont(asset_path("assets/fonts/font_basis33.json"),
                                                    asset_path("assets/fonts/font_basis33.png"));
                                if (!term_window_bottom->font) {
                                    fprintf(stderr, "Failed to load font for bottom terminal\n");
                                }
                            }

                            // Initialize bottom terminal
                            ansi_init(&ansi_term_bottom);
                            terminal_bottom.line_count = 0;
                            terminal_bottom.scroll_offset = 0;
                            terminal_bottom.cursor_pos = 0;
                            terminal_bottom.input_buffer[0] = '\0';

                            // Start separate shell for bottom terminal
                            // Calculate actual window size
                            int rows = 24, cols = 80;
                            if (term_window_bottom && term_window_bottom->font) {
                                float char_h = term_window_bottom->font_size.value * 16.0f * (1.0f + term_window_bottom->line_spacing.value);
                                float border_padding = term_window_bottom->border_size.size * 2.0f;
                                float usable_h = term_window_bottom->size.height - term_window_bottom->text_margins.top -
                                                term_window_bottom->text_margins.bottom - border_padding;

                                // Account for vertical padding (matches rendering)
                                float vertical_padding_bottom = char_h * 0.75f;
                                usable_h -= vertical_padding_bottom;

                                cols = terminal_cols_for_window(term_window_bottom);
                                rows = (int)(usable_h / char_h);
                                if (rows > 3) rows -= 3;  // Match PTY resize: 2 for safety + 1 for descender clipping
                                if (rows < 10) rows = 10;
                                if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;
                            }

                            // Autowrap at exactly the width the child is told.
                            ansi_set_size(&ansi_term_bottom, cols, rows);
                            struct winsize ws = {
                                .ws_row = rows,
                                .ws_col = cols,
                                .ws_xpixel = 0,
                                .ws_ypixel = 0
                            };

                            pty_child_pid_bottom = forkpty(&pty_master_fd_bottom, NULL, NULL, &ws);
                            if (pty_child_pid_bottom == 0) {
                                // Child process - forkpty already created session and controlling terminal
                                // DO NOT call setsid()
                                for (int fd = 3; fd < 256; fd++) close(fd);
                                setenv("TERM", "xterm", 1);
                                setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);

                                // Configure terminal for SSH compatibility
                                struct termios tios;
                                if (tcgetattr(STDIN_FILENO, &tios) == 0) {
                                    tios.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
                                    tios.c_oflag |= OPOST | ONLCR;
                                    tios.c_iflag |= ICRNL | IXON;
                                    tcsetattr(STDIN_FILENO, TCSANOW, &tios);
                                }

                                exec_default_shell();
                                _exit(1);
                            } else {
                                // Parent: set non-blocking
                                int flags = fcntl(pty_master_fd_bottom, F_GETFL, 0);
                                if (flags >= 0) {
                                    fcntl(pty_master_fd_bottom, F_SETFL, flags | O_NONBLOCK);
                                }
                                interactive_mode_bottom = true;
                                pty_start_time_bottom = glfwGetTime();

                                // Set correct terminal size for the bottom PTY
                                resize_pty_to_window_bottom();
                            }

                            // CRITICAL: Create 9-slice for bottom terminal before showing
                            NineSliceParams bottom_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window_bottom->size,
                                .borderLeft = term_window_bottom->border_size,
                                .borderRight = term_window_bottom->border_size,
                                .borderTop = term_window_bottom->border_size,
                                .borderBottom = term_window_bottom->border_size
                            };
                            createNineSlice(&term_window_bottom->nine_slice, &bottom_params, WINDOW_SIZE(32, 32), term_window_bottom->texture_path.value);

                            wm_show_window(&wm, term_window_bottom->name);
                            printf("Horizontal split enabled - two independent terminals (top/bottom)\n");
                        }
                        break;
                    case 7:  // Close Split
                        printf("Close Split selected\n");
                        if (split_horizontal) {
                            // Close vertical split (left/right)
                            printf("Closing vertical split...\n");

                            // Close right terminal PTY
                            if (pty_master_fd_right >= 0) {
                                close(pty_master_fd_right);
                                pty_master_fd_right = -1;
                            }

                            // Kill right terminal child process
                            if (pty_child_pid_right > 0) {
                                kill(pty_child_pid_right, SIGTERM);
                                waitpid(pty_child_pid_right, NULL, WNOHANG);
                                pty_child_pid_right = -1;
                            }

                            // Free right terminal font
                            if (term_window_right && term_window_right->font) {
                                FreeFont(term_window_right->font);
                                term_window_right->font = NULL;
                            }

                            // Clear right terminal buffer
                            terminal_right.line_count = 0;
                            terminal_right.raw_line_count = 0;
                            terminal_right.scroll_offset = 0;
                            terminal_right.cursor_pos = 0;
                            terminal_right.input_buffer[0] = '\0';
                            terminal_right.history_count = 0;
                            terminal_right.history_index = -1;
                            ansi_clear(&ansi_term_right);
                            ansi_scroll_offset_right = 0;

                            // Hide right window
                            if (term_window_right) {
                                wm_hide_window(&wm, term_window_right->name);
                                term_window_right = NULL;
                            }

                            // Reset split flag and focus
                            split_horizontal = false;
                            interactive_mode_right = false;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Restore main terminal to full size
                            term_window->size = WINDOW_SIZE(current_width - 40, current_height - 40);
                            term_window->anchor = ANCHOR_CENTER;
                            term_window->position = SCREEN_COORD(0, 0);

                            // CRITICAL: Recreate 9-slice for restored full-size terminal
                            NineSliceParams restore_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &restore_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap main terminal content for new width
                            rewrap_terminal_content();

                            // Resize PTY
                            resize_pty_to_window();

                            printf("Vertical split closed - returned to single terminal\n");
                        } else if (split_vertical) {
                            // Close horizontal split (top/bottom)
                            printf("Closing horizontal split...\n");

                            // Close bottom terminal PTY
                            if (pty_master_fd_bottom >= 0) {
                                close(pty_master_fd_bottom);
                                pty_master_fd_bottom = -1;
                            }

                            // Kill bottom terminal child process
                            if (pty_child_pid_bottom > 0) {
                                kill(pty_child_pid_bottom, SIGTERM);
                                waitpid(pty_child_pid_bottom, NULL, WNOHANG);
                                pty_child_pid_bottom = -1;
                            }

                            // Free bottom terminal font
                            if (term_window_bottom && term_window_bottom->font) {
                                FreeFont(term_window_bottom->font);
                                term_window_bottom->font = NULL;
                            }

                            // Clear bottom terminal buffer
                            terminal_bottom.line_count = 0;
                            terminal_bottom.raw_line_count = 0;
                            terminal_bottom.scroll_offset = 0;
                            terminal_bottom.cursor_pos = 0;
                            terminal_bottom.input_buffer[0] = '\0';
                            terminal_bottom.history_count = 0;
                            terminal_bottom.history_index = -1;
                            ansi_clear(&ansi_term_bottom);
                            ansi_scroll_offset_bottom = 0;

                            // Hide bottom window
                            if (term_window_bottom) {
                                wm_hide_window(&wm, term_window_bottom->name);
                                term_window_bottom = NULL;
                            }

                            // Reset split flag and focus
                            split_vertical = false;
                            interactive_mode_bottom = false;
                            focused_terminal = 0;

                            // Get current window size
                            int current_width, current_height;
                            glfwGetWindowSize(window, &current_width, &current_height);

                            // Restore main terminal to full size
                            term_window->size = WINDOW_SIZE(current_width - 40, current_height - 40);
                            term_window->anchor = ANCHOR_CENTER;
                            term_window->position = SCREEN_COORD(0, 0);

                            // CRITICAL: Recreate 9-slice for restored full-size terminal
                            NineSliceParams restore_params = {
                                .position = SCREEN_COORD(0, 0),
                                .size = term_window->size,
                                .borderLeft = term_window->border_size,
                                .borderRight = term_window->border_size,
                                .borderTop = term_window->border_size,
                                .borderBottom = term_window->border_size
                            };
                            createNineSlice(&term_window->nine_slice, &restore_params, WINDOW_SIZE(32, 32), term_window->texture_path.value);

                            // CRITICAL: Invalidate cache after size change
                            invalidate_line_width_cache();

                            // Rewrap main terminal content for new width
                            rewrap_terminal_content();

                            // Resize PTY
                            resize_pty_to_window();

                            printf("Horizontal split closed - returned to single terminal\n");
                        } else {
                            printf("No split to close - already in single terminal mode\n");
                        }
                        break;
                    case MENU_ITEM_SOUND:  // Sound on/off
                        sound_set_enabled(!sound_is_enabled());
                        printf("Sound %s\n", sound_is_enabled() ? "enabled" : "disabled");
                        break;
                    case 9:  // Exit
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                        break;
                }
            }
            context_menu.visible = false;
        }
    }
}

// Render context menu
void render_context_menu() {
    if (!context_menu.visible) return;

    int width  = wm.screen_size.width;
    int height = wm.screen_size.height;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    // TOP-LEFT origin: y goes down
    glOrtho(0, width, height, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    float menu_height = MENU_ITEM_COUNT * MENU_ITEM_HEIGHT;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Background
    glColor4f(0.1f, 0.1f, 0.1f, 0.95f);
    glBegin(GL_QUADS);
    glVertex2f(context_menu.x,                     context_menu.y);
    glVertex2f(context_menu.x + MENU_WIDTH,       context_menu.y);
    glVertex2f(context_menu.x + MENU_WIDTH,       context_menu.y + menu_height);
    glVertex2f(context_menu.x,                    context_menu.y + menu_height);
    glEnd();

    // Border
    glColor4f(0.6f, 0.6f, 0.6f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(context_menu.x,                     context_menu.y);
    glVertex2f(context_menu.x + MENU_WIDTH,        context_menu.y);
    glVertex2f(context_menu.x + MENU_WIDTH,        context_menu.y + menu_height);
    glVertex2f(context_menu.x,                     context_menu.y + menu_height);
    glEnd();

    // Items top-down
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        float item_y = context_menu.y + i * MENU_ITEM_HEIGHT;

        // Highlight
        if (i == context_menu.selected_item) {
            glColor4f(0.3f, 0.3f, 0.5f, 0.8f);
            glBegin(GL_QUADS);
            glVertex2f(context_menu.x,              item_y);
            glVertex2f(context_menu.x + MENU_WIDTH, item_y);
            glVertex2f(context_menu.x + MENU_WIDTH, item_y + MENU_ITEM_HEIGHT);
            glVertex2f(context_menu.x,              item_y + MENU_ITEM_HEIGHT);
            glEnd();
        }

        // Separator
        if (i > 0) {
            glColor4f(0.4f, 0.4f, 0.4f, 1.0f);
            glBegin(GL_LINES);
            glVertex2f(context_menu.x + 5,              item_y);
            glVertex2f(context_menu.x + MENU_WIDTH - 5, item_y);
            glEnd();
        }

        // Text (same index as menu_items[])
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        if (term_window && term_window->font) {
            RenderText(term_window->font,
                       menu_item_label(i),
                       context_menu.x + 10,
                       item_y + context_menu.text_y_offset,
                       1.0f,
                       1.0f);
        }
    }

    glDisable(GL_BLEND);

    glPopMatrix();                  // MODELVIEW
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

// Window resize callback
void window_resize_callback(GLFWwindow* window, int width, int height) {
    (void)window;

    // Update window manager screen size
    wm.screen_size = SCREEN_SIZE(width, height);

    // CRITICAL: Invalidate cached line width since window size changed
    invalidate_line_width_cache();

    // Update terminal window sizes
    if (split_horizontal) {
        // Recalculate horizontal split sizes (left/right) - COMPLETELY reset layout
        int usable_width = width - (WINDOW_EDGE_PADDING * 2);
        int usable_height = height - (WINDOW_EDGE_PADDING * 2);
        int half_width = (usable_width - TERMINAL_GAP) / 2;

        // The size limit should keep us well clear of this, but a window
        // manager is free to ignore it. A pane thinner than its own border
        // inverts the 9-slice quads and draws a scrambled frame.
        int min_pane = (int)(term_window ? term_window->border_size.size * 4 : 32);
        if (half_width < min_pane) half_width = min_pane;
        if (usable_height < min_pane) usable_height = min_pane;

        if (term_window) {
            // FULL RESET: size, anchor, position (as if splitting fresh)
            term_window->size = WINDOW_SIZE(half_width, usable_height);
            term_window->anchor = ANCHOR_CENTER_LEFT;
            term_window->position = SCREEN_COORD(WINDOW_EDGE_PADDING, 0);
            // Keep existing margins (user may have adjusted them)


            // Rewrap left terminal content for new width
            rewrap_terminal_content();
        }
        if (term_window_right) {
            // FULL RESET: size, anchor, position (as if splitting fresh)
            term_window_right->size = WINDOW_SIZE(half_width, usable_height);
            term_window_right->anchor = ANCHOR_CENTER_RIGHT;
            term_window_right->position = SCREEN_COORD(-WINDOW_EDGE_PADDING, 0);
            // Keep margins synchronized with left terminal
            term_window_right->text_margins = term_window->text_margins;
            term_window_right->border_size = term_window->border_size;

        }
    } else if (split_vertical) {
        // Recalculate vertical split sizes (top/bottom) - COMPLETELY reset layout
        int usable_width = width - (WINDOW_EDGE_PADDING * 2);
        int usable_height = height - (WINDOW_EDGE_PADDING * 2);
        int half_height = (usable_height - TERMINAL_GAP) / 2;

        int min_pane = (int)(term_window ? term_window->border_size.size * 4 : 32);
        if (half_height < min_pane) half_height = min_pane;
        if (usable_width < min_pane) usable_width = min_pane;

        if (term_window) {
            // FULL RESET: size, anchor, position (as if splitting fresh)
            term_window->size = WINDOW_SIZE(usable_width, half_height);
            term_window->anchor = ANCHOR_TOP_CENTER;
            term_window->position = SCREEN_COORD(0, WINDOW_EDGE_PADDING);
            // Keep existing margins (user may have adjusted them)


            // Rewrap top terminal content for new height
            rewrap_terminal_content();
        }
        if (term_window_bottom) {
            // FULL RESET: size, anchor, position (as if splitting fresh)
            term_window_bottom->size = WINDOW_SIZE(usable_width, half_height);
            term_window_bottom->anchor = ANCHOR_BOTTOM_CENTER;
            term_window_bottom->position = SCREEN_COORD(0, -WINDOW_EDGE_PADDING);
            // Keep margins synchronized with top terminal
            term_window_bottom->text_margins = term_window->text_margins;
            term_window_bottom->border_size = term_window->border_size;

        }
    } else {
        // Single terminal mode - ensure anchor/position are correct
        if (term_window) {
            term_window->size = WINDOW_SIZE(width - 40, height - 40);
            term_window->anchor = ANCHOR_CENTER;  // Reset to center
            term_window->position = SCREEN_COORD(0, 0);  // Reset to center position


            // Rewrap all text to fit new width
            rewrap_terminal_content();
        }
    }

    // Update OpenGL viewport
    glViewport(0, 0, width, height);

    // Notify PTY of resize - this updates TIOCSWINSZ for interactive programs
    resize_pty_to_window();
    if (split_horizontal) {
        resize_pty_to_window_right();
    } else if (split_vertical) {
        resize_pty_to_window_bottom();
    }

    DEBUG_PRINT("Window resized to: %dx%d\n", width, height);
}

// Set by the signal handler so the main loop can leave through the usual
// teardown. A handler cannot safely call into GLFW or free anything, so it
// does nothing but raise this flag.
static volatile sig_atomic_t shutdown_requested = 0;

static void request_shutdown(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

// Stop a PTY child and reap it. SIGHUP first, which is what a real terminal
// sends when its window goes away and what lets a shell run its exit traps;
// SIGKILL only for something that ignores it. Closing the master first also
// gives the child EOF on its stdin, so a well-behaved shell is usually gone
// before the signal arrives.
static void shutdown_pty(pid_t* pid, int* master_fd) {
    if (*master_fd != -1) {
        close(*master_fd);
        *master_fd = -1;
    }

    if (*pid <= 0) {
        *pid = -1;
        return;
    }

    kill(*pid, SIGHUP);

    // Wait briefly rather than blocking forever on a wedged child.
    for (int i = 0; i < 200; i++) {
        if (waitpid(*pid, NULL, WNOHANG) == *pid) {
            *pid = -1;
            return;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };  // 1 ms
        nanosleep(&ts, NULL);
    }

    kill(*pid, SIGKILL);
    waitpid(*pid, NULL, 0);
    *pid = -1;
}

// GLFW error callback for debugging
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int argc, char** argv) {
    (void)argc;

    // Locate the assets before anything tries to load one. This must happen
    // first: it is what lets the binary run from outside the project folder.
    asset_root_init(argv[0]);

    // A session needs either X11 or Wayland. Checking DISPLAY alone turned a
    // pure-Wayland desktop - now the default on Fedora and Ubuntu - into a
    // hard exit before GLFW ever got a chance to try its Wayland backend.
    const char* display = getenv("DISPLAY");
    const char* wayland = getenv("WAYLAND_DISPLAY");
    bool have_x11     = display && display[0] != '\0';
    bool have_wayland = wayland && wayland[0] != '\0';

    if (!have_x11 && !have_wayland) {
        fprintf(stderr, "Error: no display server found "
                        "(neither DISPLAY nor WAYLAND_DISPLAY is set).\n");
        fprintf(stderr, "If running over SSH, use: ssh -X user@host\n");
        fprintf(stderr, "If running locally, ensure X11 or Wayland is running.\n");
        return -1;
    }
    printf("Using %s=%s\n", have_x11 ? "DISPLAY" : "WAYLAND_DISPLAY",
                            have_x11 ? display : wayland);

    // Initialize GLFW with error callback for debugging
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        // Most distributions ship an X11-only GLFW (Arch's glfw-x11, and the
        // Debian/Ubuntu libglfw3 built without the Wayland backend). On a
        // Wayland session that build needs XWayland, so say so rather than
        // leaving the bare GLFW error as the only clue.
        if (!have_x11 && have_wayland) {
            fprintf(stderr, "This looks like a Wayland session with no XWayland.\n");
            fprintf(stderr, "Install XWayland, or a GLFW built with Wayland support.\n");
        }
        return -1;
    }

    // Create window - try without version hints first for maximum compatibility
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);  // Enable resizing

    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Terminal Emulator", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        fprintf(stderr, "Try running: glxinfo | grep 'OpenGL version' to check OpenGL support\n");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    printf("OpenGL context created successfully\n");

    // Swap without waiting on the compositor. The loop is paced by hand at the
    // bottom instead - see the note there for why vsync cannot do this job.
    glfwSwapInterval(0);

    glfwSetKeyCallback(window, key_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetWindowSizeCallback(window, window_resize_callback);  // Add resize callback
    glfwSetMouseButtonCallback(window, mouse_button_callback);  // Add mouse button callback
    glfwSetCursorPosCallback(window, cursor_position_callback);  // Add mouse position callback
    glfwSetScrollCallback(window, scroll_callback);  // Add scroll callback for mouse wheel

    // Initialize GLEW
    // Required on Linux/Mesa to avoid "Unknown error" on glewInit
    glewExperimental = GL_TRUE;

    // Clear any existing GL errors before GLEW init
    while (glGetError() != GL_NO_ERROR) {}

    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "GLEW init returned error code: %d\n", err);
        fprintf(stderr, "GLEW error string: %s\n", glewGetErrorString(err));

        // Try to continue anyway if we have basic OpenGL - GLEW sometimes fails
        // on indirect GLX (X11 forwarding) but basic GL functions still work
        const char* gl_version = (const char*)glGetString(GL_VERSION);
        if (gl_version) {
            fprintf(stderr, "OpenGL reports version: %s - attempting to continue...\n", gl_version);
        } else {
            fprintf(stderr, "No OpenGL version available - cannot continue\n");
            return -1;
        }
    }

    // Clear any GL errors generated by glewInit (common with glewExperimental)
    while (glGetError() != GL_NO_ERROR) {}

    printf("OpenGL Version: %s\n", glGetString(GL_VERSION));
    printf("OpenGL Renderer: %s\n", glGetString(GL_RENDERER));

    // Initialize window manager
    wm_init(&wm, SCREEN_SIZE(WINDOW_WIDTH, WINDOW_HEIGHT));

    // Create terminal window
    WindowID term_id = wm_create_window(&wm, window_name_from_string("terminal"));
    term_window = &wm.windows[term_id.value];

    // Configure terminal window (matching window editor defaults)
    term_window->anchor = ANCHOR_CENTER;
    term_window->position = SCREEN_COORD(0, 0);
    term_window->size = WINDOW_SIZE(WINDOW_WIDTH - 40, WINDOW_HEIGHT - 40);
    term_window->texture_path = font_path_from_string(asset_path("assets/ui/9-slice-basice9.png"));
    term_window->text_color = TEXT_COLOR(1.0f, 1.0f, 1.0f, 1.0f); // White text
    term_window->font_size = FONT_SIZE(1.0f);  // 16px - default size
    term_window->border_size = BORDER_SIZE(8);
    term_window->line_spacing = LINE_SPACING(0.30f);  // Slightly more spacing for terminal
    term_window->text_margins = TEXT_MARGINS(15, 25, 38, -55);  // left, right, top, bottom - Aligned with border
    term_window->center_alpha = 0.9f; // Slightly transparent center

    // Load font_basis33
    printf("Loading assets from: %s\n", asset_root[0] ? asset_root : "(current directory)");
    term_window->font = LoadFont(asset_path("assets/fonts/font_basis33.json"),
                                 asset_path("assets/fonts/font_basis33.png"));
    if (!term_window->font) {
        // Name the directory actually searched - the usual cause is a binary
        // moved away from its assets, and that is invisible otherwise.
        fprintf(stderr, "Failed to load the font from %s\n",
                asset_path("assets/fonts/"));
        fprintf(stderr, "The assets/ folder must sit next to the terminal binary.\n");
        return -1;
    }
    printf("Font loaded successfully!\n");
    printf("Font texture ID: %u\n", term_window->font->texture);
    printf("Font first_char: %d, last_char: %d\n", term_window->font->first_char, term_window->font->last_char);

    // Show terminal window
    wm_show_window(&wm, term_window->name);

    // Initialize terminal
    terminal_init();

    // Keep the window big enough for whatever layout is on screen.
    apply_window_size_limits(window);

    // Optional: the terminal runs fine with no audio device.
    if (!sound_init(asset_path(ENTER_SOUND_PATH))) {
        printf("Sound disabled (no clip or no audio device)\n");
    }

    // bash is the default environment: bring it up straight away rather than
    // making the user ask for it. If neither bash nor sh can start, the
    // built-in line shell initialised above is still there as a fallback.
    start_interactive_shell(NULL);

    printf("Terminal emulator started with font_basis33!\n");
    printf("Controls:\n");
    printf("  Ctrl+Q   - Exit terminal\n");
    printf("  F4/F5    - Adjust top margin (down/up)\n");
    printf("  F6/F7    - Adjust bottom margin (extend/reduce)\n");
    printf("  F8/F9    - Font size (increase/decrease)\n");
    printf("  F10/F11  - Adjust left margin (right/left)\n");

    // Leave by the same route however we are asked to. Without this,
    // SIGINT/SIGTERM/SIGHUP kill the process outright: the child shells are
    // never signalled, the ALSA device is left open, and the teardown below
    // never runs.
    //
    // This has to happen here, not early in main(). Pulling in the font and
    // image stack starts glib (the gmain/gdbus/dconf threads), and it installs
    // handlers of its own during init - anything registered beforehand gets
    // replaced. Registering last is what makes ours the one that runs.
    //
    // No SA_RESTART: the loop can be parked in poll() inside the compositor's
    // frame wait, and restarting that syscall would swallow the signal until
    // the next frame - which never arrives if the window is hidden. Letting it
    // fail with EINTR is what wakes the loop up to see the flag.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = request_shutdown;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP,  &sa, NULL);
    }

    // Main loop
    double last_time = glfwGetTime();
    while (!glfwWindowShouldClose(window) && !shutdown_requested) {
        double frame_start = glfwGetTime();

        // LOW LATENCY: Poll events FIRST to minimize input-to-frame delay
        glfwPollEvents();

        double current_time = glfwGetTime();
        float delta_time = (float)(current_time - last_time);
        last_time = current_time;

        // Update cursor blink for all terminals (both old and ANSI terminals)
        terminal.cursor_blink_timer += delta_time;
        if (terminal.cursor_blink_timer > 0.5f) {
            terminal.cursor_blink_timer = 0.0f;

            // Toggle cursor for old terminal system
            terminal.cursor_visible = !terminal.cursor_visible;

            // Toggle cursor for ANSI terminals (used by interactive mode)
            ansi_term.cursor_visible = !ansi_term.cursor_visible;

            // Also update split terminal cursors
            if (split_horizontal) {
                terminal_right.cursor_visible = !terminal_right.cursor_visible;
                ansi_term_right.cursor_visible = !ansi_term_right.cursor_visible;
            } else if (split_vertical) {
                terminal_bottom.cursor_visible = !terminal_bottom.cursor_visible;
                ansi_term_bottom.cursor_visible = !ansi_term_bottom.cursor_visible;
            }
        }

        // The minimum window size depends on the split mode and the font
        // size, both of which change from several places (menu items, F8/F9).
        // Watching the resulting layout here reapplies the limit once for any
        // of them, instead of hoping every call site remembers to.
        {
            static int last_layout = -1;
            static float last_font_size = -1.0f;

            int layout = (split_horizontal ? 1 : 0) | (split_vertical ? 2 : 0);
            float font_now = term_window ? term_window->font_size.value : 1.0f;

            if (layout != last_layout || font_now != last_font_size) {
                last_layout = layout;
                last_font_size = font_now;
                apply_window_size_limits(window);
            }
        }

        // Poll PTY for output
        poll_pty();

        // Poll second terminal PTY if split
        if (split_horizontal) {
            poll_pty_right();
        } else if (split_vertical) {
            poll_pty_bottom();
        }

        // Clear screen
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render window manager (renders the 9-slice border)
        wm_render(&wm);

        // Render terminal content on top
        render_terminal_content();

        // Render second terminal if split
        if (split_horizontal) {
            render_terminal_content_right();
        } else if (split_vertical) {
            render_terminal_content_bottom();
        }

        // Render context menu (if visible)
        render_context_menu();

        glfwSwapBuffers(window);
        // Note: glfwPollEvents() moved to top of loop for lower latency

        // Pace the loop to ~60 FPS.
        //
        // Left to run flat out it renders thousands of frames a second and
        // commits a surface for each one. The compositor consumes them at the
        // refresh rate at best, and libwayland-client queues every one that is
        // not consumed: ~11,000 allocations a second, around 2.5 MB/s, growing
        // until the OOM killer ends the process with a bare "Killed". Capping
        // the rate is what bounds that queue.
        //
        // Vsync looks like the obvious way to do this and is the wrong tool:
        // glfwSwapBuffers() would then block until the compositor sends a frame
        // callback, and it sends none to a surface that is not on screen. A
        // hidden, minimised, or never-mapped window parks the loop forever -
        // no PTY reads, no input, and no chance to notice a shutdown signal.
        // Measured here: with vsync on, the loop blocked in its first swap and
        // never completed a second iteration.
        //
        // Sleeping out the remainder of the frame keeps the rate bounded while
        // control stays with us. A signal cuts the sleep short, which is what
        // makes shutdown prompt.
        {
            const double frame_budget = 1.0 / 60.0;
            double spent = glfwGetTime() - frame_start;

            if (spent < frame_budget) {
                double remaining = frame_budget - spent;
                struct timespec ts = {
                    .tv_sec  = (time_t)remaining,
                    .tv_nsec = (long)((remaining - (double)(time_t)remaining) * 1e9)
                };
                nanosleep(&ts, NULL);
            }
        }
    }

    // Cleanup. The children go first: once this process exits they would be
    // reparented to init and left running with no terminal attached, which is
    // how a session leaks a stray shell every time the window closes.
    shutdown_pty(&pty_child_pid,        &pty_master_fd);
    shutdown_pty(&pty_child_pid_right,  &pty_master_fd_right);
    shutdown_pty(&pty_child_pid_bottom, &pty_master_fd_bottom);

    // Fonts hold a GL texture and a vertex buffer each, so they have to go
    // before the context does.
    if (term_window && term_window->font) {
        FreeFont(term_window->font);
        term_window->font = NULL;
    }
    if (term_window_right && term_window_right->font) {
        FreeFont(term_window_right->font);
        term_window_right->font = NULL;
    }
    if (term_window_bottom && term_window_bottom->font) {
        FreeFont(term_window_bottom->font);
        term_window_bottom->font = NULL;
    }

    sound_shutdown();
    wm_cleanup(&wm);
    glfwDestroyWindow(window);
    glfwTerminate();

    printf("Terminal closed cleanly\n");
    return 0;
}
