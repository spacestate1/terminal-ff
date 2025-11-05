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
#include <pty.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <errno.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "window_manager.h"
#include "font-render.h"
#include "ansi.h"

#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
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

// Context menu
typedef struct {
    bool visible;
    float x, y;
    int selected_item;
    double mouse_x, mouse_y;
    float text_y_offset;  // Adjustable Y offset for text positioning
} ContextMenu;

ContextMenu context_menu = {false, 0, 0, -1, 0, 0, 22.0f};  // Default offset = 22

#define MENU_ITEM_HEIGHT 30.0f
#define MENU_WIDTH 180.0f

const char* menu_items[] = {
    "Copy",
    "Paste",
    "Clear Screen",
    "Font Size +",
    "Font Size -",
    "Exit"
};
#define MENU_ITEM_COUNT 6

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

// Load texture function (for 9-slice borders)
GLuint loadTexture(const char* path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(0);  // Don't flip - 9-slice coordinates expect non-flipped
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);

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

    GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);
    printf("Loaded texture: %s (%dx%d, %d channels)\n", path, width, height, nrChannels);

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

        // Store in history
        strncpy(terminal.history[terminal.history_count], line, MAX_LINE_LENGTH - 1);
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
void start_interactive_shell(const char* cmd) {
    if (pty_master_fd != -1) return; // already running

    struct winsize ws = {
        .ws_row = ANSI_BUFFER_ROWS,
        .ws_col = ANSI_BUFFER_COLS,
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

        // Create a new session - completely isolated from parent terminal
        setsid();

        // The PTY slave is already stdin/stdout/stderr thanks to forkpty
        // Close any other inherited file descriptors
        for (int fd = 3; fd < 256; fd++) {
            close(fd);
        }

        setenv("TERM", "xterm-256color", 1);

        // Suppress terminal capability warnings during bash startup
        setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);

        if (cmd && *cmd) {
            execlp("sh", "sh", "-c", cmd, (char*)NULL);
        } else {
            // Use -i (interactive) instead of --login to avoid terminal setting warnings
            // The PTY provides a proper terminal environment
            execlp("bash", "bash", "-i", (char*)NULL);
        }

        // If execlp fails
        perror("execlp");
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
    const int max_reads = 100; // Prevent infinite loop

    for (;;) {
        if (read_count++ > max_reads) {
            // Safety: prevent getting stuck in read loop
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
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // no more data right now
            break;
        } else if (n == 0) {
            // child exited - reap the zombie process
            if (pty_child_pid > 0) {
                int status;
                waitpid(pty_child_pid, &status, WNOHANG);
                pty_child_pid = -1;
            }
            close(pty_master_fd);
            pty_master_fd = -1;
            interactive_mode = false;
            terminal_add_line("[process exited]");
            break;
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

        // Home/End/Page keys
        case GLFW_KEY_HOME:      seq = "\x1b[H"; break;
        case GLFW_KEY_END:       seq = "\x1b[F"; break;
        case GLFW_KEY_PAGE_UP:   seq = "\x1b[5~"; break;
        case GLFW_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
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
    float char_w = GetAverageCharWidth(term_window->font, term_window->font_size.value);
    float char_h = term_window->font_size.value * 16.0f * (1.0f + term_window->line_spacing.value);

    // Text content area inside window (respect margins + border padding)
    float border_padding = term_window->border_size.size * 2.0f;
    float usable_w = term_window->size.width
                   - term_window->text_margins.left
                   - term_window->text_margins.right
                   - border_padding;
    float usable_h = term_window->size.height
                   - term_window->text_margins.top
                   - term_window->text_margins.bottom
                   - border_padding;

    // Account for vertical padding for first line (0.75 * line_height)
    float vertical_padding = char_h * 0.75f;
    usable_h -= vertical_padding;

    int cols = (int)(usable_w / char_w);
    int rows = (int)(usable_h / char_h);

    // Subtract 3 columns for safety margin (matches rendering calculation)
    if (cols > 3) cols -= 3;

    if (cols < 20) cols = 20;
    if (rows < 10) rows = 10;  // Ensure minimum rows for programs like top

    // Don't advertise more than we can store in ansi_term
    if (cols > ANSI_BUFFER_COLS) cols = ANSI_BUFFER_COLS;
    if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    printf("PTY size: %d rows × %d cols (usable: %.0f×%.0f, char: %.1f×%.1f)\n",
           rows, cols, usable_w, usable_h, char_w, char_h);
    fflush(stdout);

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
    terminal_add_line("Type 'help' or 'shell' for interactive shell");
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
int calculate_line_width() {
    if (!term_window || !term_window->font) return 80;  // Default

    // Calculate available width - must match rendering boundaries exactly
    float border_padding = term_window->border_size.size * 2.0f;  // 8px on each side = 16px total
    float available_width = term_window->size.width -
                           term_window->text_margins.left -
                           term_window->text_margins.right -
                           border_padding;  // No extra margin - match rendering exactly

    // Get actual character width from font metrics
    float char_width = GetAverageCharWidth(term_window->font, term_window->font_size.value);

    int chars_per_line = (int)(available_width / char_width);

    // Leave one column visually empty for extra padding (match PTY calculation)
    if (chars_per_line > 1) chars_per_line -= 1;

    // Ensure minimum width but respect actual space
    if (chars_per_line < 20) chars_per_line = 20;  // Minimum readable width
    if (available_width < 100) chars_per_line = 10;  // Very narrow window fallback

    return chars_per_line;
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
void rewrap_terminal_content() {
    if (!term_window) return;

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
void terminal_add_line(const char* text) {
    // Add to raw lines
    if (terminal.raw_line_count >= MAX_TERMINAL_LINES) {
        // Shift raw lines up
        for (int i = 0; i < MAX_TERMINAL_LINES - 1; i++) {
            strcpy(terminal.raw_lines[i], terminal.raw_lines[i + 1]);
        }
        terminal.raw_line_count = MAX_TERMINAL_LINES - 1;
    }

    // Strip ANSI codes before adding
    char clean_text[MAX_LINE_LENGTH];
    strip_ansi_codes(clean_text, text, MAX_LINE_LENGTH);

    strncpy(terminal.raw_lines[terminal.raw_line_count], clean_text, MAX_LINE_LENGTH - 1);
    terminal.raw_lines[terminal.raw_line_count][MAX_LINE_LENGTH - 1] = '\0';
    terminal.raw_line_count++;

    // Rewrap everything to update display
    rewrap_terminal_content();
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
void handle_tab_completion() {
    if (terminal.cursor_pos == 0) return;

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
        char* last_slash = strrchr(temp, '/');
        if (last_slash) {
            *last_slash = '\0';
            strncpy(dir_path, temp[0] ? temp : "/", sizeof(dir_path) - 1);
            strncpy(prefix, last_slash + 1, sizeof(prefix) - 1);
        }
    } else {
        strncpy(prefix, word_start, sizeof(prefix) - 1);
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
        strncpy(new_input, terminal.input_buffer, word_offset);
        new_input[word_offset] = '\0';

        if (is_path) {
            // Keep the directory path
            char* last_slash = strrchr(word_start, '/');
            if (last_slash) {
                strncat(new_input, word_start, (last_slash - word_start) + 1);
            }
        }

        strncat(new_input, matches[0], MAX_LINE_LENGTH - strlen(new_input) - 1);

        strncpy(terminal.input_buffer, new_input, MAX_LINE_LENGTH - 1);
        terminal.cursor_pos = strlen(terminal.input_buffer);

        // Echo completion with prompt
        char echo_line[MAX_LINE_LENGTH * 2];
        snprintf(echo_line, sizeof(echo_line), "%s%s", terminal.prompt, terminal.input_buffer);
        terminal_add_line(echo_line);
    } else {
        // Multiple matches - show them
        terminal_add_line("");
        char line[MAX_LINE_LENGTH];
        for (int i = 0; i < match_count; i++) {
            if (i % 3 == 0) {
                if (i > 0) terminal_add_line(line);
                snprintf(line, sizeof(line), "  %-25s", matches[i]);
            } else {
                char temp[30];
                snprintf(temp, sizeof(temp), "%-25s", matches[i]);
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
            strncpy(new_input, terminal.input_buffer, word_offset);
            new_input[word_offset] = '\0';

            if (is_path) {
                char* last_slash = strrchr(word_start, '/');
                if (last_slash) {
                    strncat(new_input, word_start, (last_slash - word_start) + 1);
                }
            }

            strncat(new_input, matches[0], common_len);
            new_input[word_offset + common_len] = '\0';

            strncpy(terminal.input_buffer, new_input, MAX_LINE_LENGTH - 1);
            terminal.cursor_pos = strlen(terminal.input_buffer);
        }
    }
}

// Keyboard input callback
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

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
            if (terminal.cursor_pos > 0) {
                int remaining = strlen(terminal.input_buffer) - terminal.cursor_pos;
                memmove(terminal.input_buffer, terminal.input_buffer + terminal.cursor_pos, remaining + 1);
                terminal.cursor_pos = 0;
            }
            return;
        }
        if (key == GLFW_KEY_K) {
            // Ctrl+K: Kill line (delete from cursor to end)
            terminal.input_buffer[terminal.cursor_pos] = '\0';
            return;
        }
        if (key == GLFW_KEY_W) {
            // Ctrl+W: Delete word backwards
            if (terminal.cursor_pos > 0) {
                int start = terminal.cursor_pos;
                // Skip trailing spaces
                while (start > 0 && terminal.input_buffer[start - 1] == ' ') {
                    start--;
                }
                // Delete word
                while (start > 0 && terminal.input_buffer[start - 1] != ' ') {
                    start--;
                }
                int remaining = strlen(terminal.input_buffer) - terminal.cursor_pos;
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
        term_window->font_size.value += 0.1f;
        if (term_window->font_size.value > 5.0f) term_window->font_size.value = 5.0f;  // Max size
        printf("Font size: %.1f (%.0fpx)\n", term_window->font_size.value, term_window->font_size.value * 16.0f);
        fflush(stdout);
        resize_pty_to_window();
        rewrap_terminal_content();
        return;
    }
    if (key == GLFW_KEY_F9) {
        term_window->font_size.value -= 0.1f;
        if (term_window->font_size.value < 0.5f) term_window->font_size.value = 0.5f;  // Min size
        printf("Font size: %.1f (%.0fpx)\n", term_window->font_size.value, term_window->font_size.value * 16.0f);
        fflush(stdout);
        resize_pty_to_window();
        rewrap_terminal_content();
        return;
    }
    if (key == GLFW_KEY_F10) {
        term_window->text_margins.left += 1;
        printf("Left margin: %d (text moved right)\n", (int)term_window->text_margins.left);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }
    if (key == GLFW_KEY_F11) {
        term_window->text_margins.left -= 1;
        printf("Left margin: %d (text moved left)\n", (int)term_window->text_margins.left);
        fflush(stdout);
        resize_pty_to_window();
        return;
    }

    // Interactive mode - send all keys to PTY (including ESC for vim)
    if (interactive_mode && pty_master_fd >= 0) {
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
        if (terminal.cursor_pos > 0) {
            // Delete character before cursor
            int len = strlen(terminal.input_buffer);
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
        if (terminal.cursor_pos < len) {
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
        if (terminal.history_count == 0) return;

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
        terminal.scroll_offset = (terminal.scroll_offset > 10) ? terminal.scroll_offset - 10 : 0;
        return;
    }

    if (key == GLFW_KEY_PAGE_DOWN) {
        // Calculate max visible lines for bounds checking
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
        return;
    }
}

// Character input callback
void char_callback(GLFWwindow* window, unsigned int codepoint) {
    (void)window;

    if (interactive_mode && pty_master_fd >= 0) {
        char utf8[4];
        int len = 0;

        // Only ASCII for now
        if (codepoint < 128) {
            utf8[0] = (char)codepoint;
            len = 1;
        } else {
            return;
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

    // Insert character at cursor position
    int len = strlen(terminal.input_buffer);
    if (len < MAX_LINE_LENGTH - 1) {
        // Make room for new character by shifting everything after cursor right
        memmove(terminal.input_buffer + terminal.cursor_pos + 1,
               terminal.input_buffer + terminal.cursor_pos,
               len - terminal.cursor_pos + 1);
        // Insert character
        terminal.input_buffer[terminal.cursor_pos] = (char)codepoint;
        terminal.cursor_pos++;
        terminal.history_index = -1;  // Reset history browsing when typing
    }
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
    glEnable(GL_SCISSOR_TEST);
    glScissor(
        (int)content_left,
        (int)(wm.screen_size.height - content_bottom),  // OpenGL scissor is from bottom
        (int)(content_right - content_left),
        (int)(content_bottom - content_top)
    );

    if (interactive_mode) {
        // Calculate how many columns fit horizontally (must match PTY column calculation)
        float border_padding_total = term_window->border_size.size * 2.0f;
        float usable_w = term_window->size.width -
                        term_window->text_margins.left -
                        term_window->text_margins.right -
                        border_padding_total;

        float char_w = GetAverageCharWidth(term_window->font, term_window->font_size.value);
        int max_cols = (int)(usable_w / char_w);

        // Subtract 3 columns for safety margin to ensure text never extends past border
        if (max_cols > 3) max_cols -= 3;
        if (max_cols < 20) max_cols = 20;
        if (max_cols > ANSI_BUFFER_COLS) max_cols = ANSI_BUFFER_COLS;

        // How many rows fit vertically? Account for top padding we added
        float vertical_padding = line_height * 0.75f;
        int max_rows = (int)((content_bottom - content_top - vertical_padding) / line_height);
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

        for (int row = 0; row < max_rows; row++) {
            char line[ANSI_BUFFER_COLS + 1];
            // Limit line retrieval to max_cols to prevent text extending past boundaries
            ansi_get_line(&ansi_term, row, line, max_cols + 1);

            if (line[0] != '\0') {
                RenderText(term_window->font, line,
                           x, y,
                           term_window->font_size.value,
                           term_window->line_spacing.value);
            }
            y += line_height;
        }

        glDisable(GL_SCISSOR_TEST);
        return;
    }

    // Non-interactive mode - scrollback rendering
    float available_height = content_bottom - content_top - line_height; // Reserve space for input prompt (1 line)
    int max_visible_lines = (int)(available_height / line_height);
    if (max_visible_lines < 1) max_visible_lines = 1;

    int start_line = terminal.scroll_offset;
    int end_line = (terminal.line_count < start_line + max_visible_lines - 1) ?
                    terminal.line_count : start_line + max_visible_lines - 1;

    for (int i = start_line; i < end_line; i++) {
        if (i >= 0 && i < terminal.line_count) {
            RenderText(term_window->font, terminal.lines[i], x, y,
                      term_window->font_size.value, term_window->line_spacing.value);
            y += line_height;
        }
    }

    // Render input prompt with cursor at correct position
    char prompt_line[MAX_LINE_LENGTH * 2];

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

    RenderText(term_window->font, prompt_line, x, y,
              term_window->font_size.value, term_window->line_spacing.value);

    glDisable(GL_SCISSOR_TEST);
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
        if (context_menu.visible) {
            if (context_menu.selected_item >= 0) {
                // Handle menu item click
                switch (context_menu.selected_item) {
                    case 0:  // Copy
                        // TODO: Implement clipboard copy
                        printf("Copy selected\n");
                        break;
                    case 1:  // Paste
                        // TODO: Implement clipboard paste
                        printf("Paste selected\n");
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
                    case 5:  // Exit
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
                       menu_items[i],
                       context_menu.x + 10,
                       item_y + context_menu.text_y_offset,
                       0.8f,
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

    // Update terminal window size
    if (term_window) {
        term_window->size = WINDOW_SIZE(width - 40, height - 40);

        // Rewrap all text to fit new width
        rewrap_terminal_content();
    }

    // Update OpenGL viewport
    glViewport(0, 0, width, height);

    // Notify PTY of resize
    resize_pty_to_window();

    printf("Window resized to: %dx%d\n", width, height);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Initialize GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    // Create window
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);  // Enable resizing

    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Terminal Emulator", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, key_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetWindowSizeCallback(window, window_resize_callback);  // Add resize callback
    glfwSetMouseButtonCallback(window, mouse_button_callback);  // Add mouse button callback
    glfwSetCursorPosCallback(window, cursor_position_callback);  // Add mouse position callback

    // Initialize GLEW
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW: %s\n", glewGetErrorString(err));
        return -1;
    }

    printf("OpenGL Version: %s\n", glGetString(GL_VERSION));

    // Initialize window manager
    wm_init(&wm, SCREEN_SIZE(WINDOW_WIDTH, WINDOW_HEIGHT));

    // Create terminal window
    WindowID term_id = wm_create_window(&wm, window_name_from_string("terminal"));
    term_window = &wm.windows[term_id.value];

    // Configure terminal window (matching window editor defaults)
    term_window->anchor = ANCHOR_CENTER;
    term_window->position = SCREEN_COORD(0, 0);
    term_window->size = WINDOW_SIZE(WINDOW_WIDTH - 40, WINDOW_HEIGHT - 40);
    term_window->texture_path = font_path_from_string("assets/ui/9-slice-basice9.png");
    term_window->text_color = TEXT_COLOR(1.0f, 1.0f, 1.0f, 1.0f); // White text
    term_window->font_size = FONT_SIZE(1.0f);  // 16px - default size
    term_window->border_size = BORDER_SIZE(8);
    term_window->line_spacing = LINE_SPACING(0.30f);  // Slightly more spacing for terminal
    term_window->text_margins = TEXT_MARGINS(25, 25, 38, -40);  // left, right, top, bottom - Aligned with border
    term_window->center_alpha = 0.9f; // Slightly transparent center

    // Load font_basis33
    printf("Loading font from: assets/fonts/font_basis33.json\n");
    printf("Loading texture from: assets/fonts/font_basis33.png\n");
    term_window->font = LoadFont("assets/fonts/font_basis33.json", "assets/fonts/font_basis33.png");
    if (!term_window->font) {
        fprintf(stderr, "Failed to load font_basis33!\n");
        return -1;
    }
    printf("Font loaded successfully!\n");
    printf("Font texture ID: %u\n", term_window->font->texture);
    printf("Font first_char: %d, last_char: %d\n", term_window->font->first_char, term_window->font->last_char);

    // Show terminal window
    wm_show_window(&wm, term_window->name);

    // Initialize terminal
    terminal_init();

    printf("Terminal emulator started with font_basis33!\n");
    printf("Controls:\n");
    printf("  Ctrl+Q   - Exit terminal\n");
    printf("  F4/F5    - Adjust top margin (down/up)\n");
    printf("  F6/F7    - Adjust bottom margin (extend/reduce)\n");
    printf("  F8/F9    - Font size (increase/decrease)\n");
    printf("  F10/F11  - Adjust left margin (right/left)\n");

    // Main loop
    double last_time = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        double current_time = glfwGetTime();
        float delta_time = (float)(current_time - last_time);
        last_time = current_time;

        // Update cursor blink
        terminal.cursor_blink_timer += delta_time;
        if (terminal.cursor_blink_timer > 0.5f) {
            terminal.cursor_visible = !terminal.cursor_visible;
            terminal.cursor_blink_timer = 0.0f;
        }

        // Poll PTY for output
        poll_pty();

        // Clear screen
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render window manager (renders the 9-slice border)
        wm_render(&wm);

        // Render terminal content on top
        render_terminal_content();

        // Render context menu (if visible)
        render_context_menu();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    wm_cleanup(&wm);
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
