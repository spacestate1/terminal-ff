#include "ansi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// ANSI color codes
static const unsigned char ansi_colors[] = {
    0,   // Black
    1,   // Red
    2,   // Green
    3,   // Yellow
    4,   // Blue
    5,   // Magenta
    6,   // Cyan
    7,   // White
    8,   // Bright black (gray)
    9,   // Bright red
    10,  // Bright green
    11,  // Bright yellow
    12,  // Bright blue
    13,  // Bright magenta
    14,  // Bright cyan
    15   // Bright white
};

void ansi_init(AnsiTerminal* term) {
    memset(term, 0, sizeof(AnsiTerminal));
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->scroll_top = 0;
    term->scroll_bottom = ANSI_BUFFER_ROWS - 1;
    term->current_fg = 7;  // Default white
    term->current_bg = 0;  // Default black
    term->cursor_visible = true;
    term->dirty = true;

    // Clear buffer
    for (int y = 0; y < ANSI_BUFFER_ROWS; y++) {
        for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
            term->buffer[y][x].character = ' ';
            term->buffer[y][x].fg_color = 7;
            term->buffer[y][x].bg_color = 0;
            term->buffer[y][x].bold = false;
            term->buffer[y][x].underline = false;
        }
    }
}

// Map VT100 alternate character set (DEC Special Graphics) to ASCII box-drawing
// This is used when in ACS mode (ESC ( 0) or after SO (0x0E)
// Comprehensive mapping for ncurses programs like top, htop, vim, etc.
static char acs_to_ascii(char c) {
    switch (c) {
        // Box-drawing characters (most common in ncurses)
        case 'j': return '+';  // ┘ lower right corner
        case 'k': return '+';  // ┐ upper right corner
        case 'l': return '+';  // ┌ upper left corner
        case 'm': return '+';  // └ lower left corner
        case 'n': return '+';  // ┼ cross/intersection
        case 'q': return '-';  // ─ horizontal line (most common!)
        case 't': return '+';  // ├ left tee (branch right)
        case 'u': return '+';  // ┤ right tee (branch left)
        case 'v': return '+';  // ┴ bottom tee (branch up)
        case 'w': return '+';  // ┬ top tee (branch down)
        case 'x': return '|';  // │ vertical line (most common!)

        // Additional VT100 special graphics characters
        case 'a': return '#';  // ▒ checker board (stipple)
        case 'f': return 'o';  // ° degree symbol
        case 'g': return '+';  // ± plus-minus
        case 'h': return '#';  // ▒ board of squares
        case 'i': return '#';  // ⁋ lantern symbol
        case 'o': return '-';  // ⎺ scan line 1
        case 'p': return '-';  // ⎻ scan line 3
        case 'r': return '-';  // ⎼ scan line 7
        case 's': return '_';  // ⎽ scan line 9
        case '`': return '+';  // ◆ diamond
        case '~': return 'o';  // ● bullet
        case ',': return '<';  // ← arrow pointing left
        case '+': return '>';  // → arrow pointing right
        case '.': return 'v';  // ↓ arrow pointing down
        case '-': return '^';  // ↑ arrow pointing up
        case '0': return '#';  // █ solid block

        default: return c;      // pass through unchanged
    }
}

static void ansi_putchar(AnsiTerminal* term, char c) {
    // Robust bounds checking - clamp to valid range
    if (term->cursor_y < 0) term->cursor_y = 0;
    if (term->cursor_y >= ANSI_BUFFER_ROWS) {
        term->cursor_y = ANSI_BUFFER_ROWS - 1;
    }
    if (term->cursor_x < 0) term->cursor_x = 0;
    if (term->cursor_x >= ANSI_BUFFER_COLS) {
        term->cursor_x = ANSI_BUFFER_COLS - 1;
    }

    // If in ACS mode, convert character
    if (term->acs_mode) {
        c = acs_to_ascii(c);
    }

    // Double-check bounds before buffer access (safety)
    if (term->cursor_y >= 0 && term->cursor_y < ANSI_BUFFER_ROWS &&
        term->cursor_x >= 0 && term->cursor_x < ANSI_BUFFER_COLS) {
        term->buffer[term->cursor_y][term->cursor_x].character = c;
        term->buffer[term->cursor_y][term->cursor_x].fg_color = term->current_fg;
        term->buffer[term->cursor_y][term->cursor_x].bg_color = term->current_bg;
        term->buffer[term->cursor_y][term->cursor_x].bold = term->current_bold;
        term->buffer[term->cursor_y][term->cursor_x].underline = term->current_underline;
    }

    term->cursor_x++;
    if (term->cursor_x >= ANSI_BUFFER_COLS) {
        term->cursor_x = 0;
        term->cursor_y++;
    }
    term->dirty = true;
}

static void ansi_newline(AnsiTerminal* term) {
    term->cursor_x = 0;
    term->cursor_y++;

    // Scroll if needed
    if (term->cursor_y >= ANSI_BUFFER_ROWS) {
        // Shift all lines up
        for (int y = 0; y < ANSI_BUFFER_ROWS - 1; y++) {
            memcpy(term->buffer[y], term->buffer[y + 1], sizeof(AnsiCell) * ANSI_BUFFER_COLS);
        }
        // Clear last line
        for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
            term->buffer[ANSI_BUFFER_ROWS - 1][x].character = ' ';
            term->buffer[ANSI_BUFFER_ROWS - 1][x].fg_color = term->current_fg;
            term->buffer[ANSI_BUFFER_ROWS - 1][x].bg_color = term->current_bg;
        }
        term->cursor_y = ANSI_BUFFER_ROWS - 1;
    }
    term->dirty = true;
}

static void ansi_carriage_return(AnsiTerminal* term) {
    term->cursor_x = 0;
}

static void ansi_clear_screen(AnsiTerminal* term, int mode) {
    switch (mode) {
        case 0:  // Clear from cursor to end
            for (int x = term->cursor_x; x < ANSI_BUFFER_COLS; x++) {
                term->buffer[term->cursor_y][x].character = ' ';
            }
            for (int y = term->cursor_y + 1; y < ANSI_BUFFER_ROWS; y++) {
                for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
                    term->buffer[y][x].character = ' ';
                }
            }
            break;
        case 1:  // Clear from start to cursor
            for (int y = 0; y < term->cursor_y; y++) {
                for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
                    term->buffer[y][x].character = ' ';
                }
            }
            for (int x = 0; x <= term->cursor_x; x++) {
                term->buffer[term->cursor_y][x].character = ' ';
            }
            break;
        case 2:  // Clear entire screen
        case 3:  // Clear entire screen and scrollback
            for (int y = 0; y < ANSI_BUFFER_ROWS; y++) {
                for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
                    term->buffer[y][x].character = ' ';
                    term->buffer[y][x].fg_color = term->current_fg;
                    term->buffer[y][x].bg_color = term->current_bg;
                }
            }
            term->cursor_x = 0;
            term->cursor_y = 0;
            break;
    }
    term->dirty = true;
}

static void ansi_clear_line(AnsiTerminal* term, int mode) {
    switch (mode) {
        case 0:  // Clear from cursor to end of line
            for (int x = term->cursor_x; x < ANSI_BUFFER_COLS; x++) {
                term->buffer[term->cursor_y][x].character = ' ';
                term->buffer[term->cursor_y][x].fg_color = term->current_fg;
                term->buffer[term->cursor_y][x].bg_color = term->current_bg;
            }
            break;
        case 1:  // Clear from start of line to cursor
            for (int x = 0; x <= term->cursor_x; x++) {
                term->buffer[term->cursor_y][x].character = ' ';
                term->buffer[term->cursor_y][x].fg_color = term->current_fg;
                term->buffer[term->cursor_y][x].bg_color = term->current_bg;
            }
            break;
        case 2:  // Clear entire line
            for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
                term->buffer[term->cursor_y][x].character = ' ';
                term->buffer[term->cursor_y][x].fg_color = term->current_fg;
                term->buffer[term->cursor_y][x].bg_color = term->current_bg;
            }
            break;
    }
    term->dirty = true;
}

static void ansi_set_cursor(AnsiTerminal* term, int row, int col) {
    term->cursor_y = row < 0 ? 0 : (row >= ANSI_BUFFER_ROWS ? ANSI_BUFFER_ROWS - 1 : row);
    term->cursor_x = col < 0 ? 0 : (col >= ANSI_BUFFER_COLS ? ANSI_BUFFER_COLS - 1 : col);
}

static void ansi_cursor_up(AnsiTerminal* term, int n) {
    term->cursor_y -= n;
    if (term->cursor_y < 0) term->cursor_y = 0;
}

static void ansi_cursor_down(AnsiTerminal* term, int n) {
    term->cursor_y += n;
    if (term->cursor_y >= ANSI_BUFFER_ROWS) term->cursor_y = ANSI_BUFFER_ROWS - 1;
}

static void ansi_cursor_forward(AnsiTerminal* term, int n) {
    term->cursor_x += n;
    if (term->cursor_x >= ANSI_BUFFER_COLS) term->cursor_x = ANSI_BUFFER_COLS - 1;
}

static void ansi_cursor_back(AnsiTerminal* term, int n) {
    term->cursor_x -= n;
    if (term->cursor_x < 0) term->cursor_x = 0;
}

static void ansi_process_sgr(AnsiTerminal* term, int* params, int count) {
    for (int i = 0; i < count; i++) {
        int param = params[i];

        switch (param) {
            case 0:  // Reset
                term->current_fg = 7;
                term->current_bg = 0;
                term->current_bold = false;
                term->current_underline = false;
                break;
            case 1:  // Bold
                term->current_bold = true;
                break;
            case 4:  // Underline
                term->current_underline = true;
                break;
            case 22:  // Normal intensity
                term->current_bold = false;
                break;
            case 24:  // Not underlined
                term->current_underline = false;
                break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:  // Foreground color
                term->current_fg = param - 30;
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:  // Background color
                term->current_bg = param - 40;
                break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:  // Bright foreground
                term->current_fg = param - 90 + 8;
                break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:  // Bright background
                term->current_bg = param - 100 + 8;
                break;
        }
    }
}

void ansi_process_output(AnsiTerminal* term, const char* data, int length) {
    for (int i = 0; i < length; i++) {
        char c = data[i];

        // Handle escape sequences
        if (c == '\033' || c == '\x1b') {
            // Need at least one more character to determine sequence type
            if (i + 1 >= length) continue;

            char next = data[i + 1];

            // Handle character set selection: ESC ( X or ESC ) X
            if ((next == '(' || next == ')') && i + 2 < length) {
                char charset = data[i + 2];
                if (charset == '0') {
                    // Enable line drawing character set (ACS mode)
                    term->acs_mode = true;
                } else if (charset == 'B') {
                    // Return to normal ASCII character set
                    term->acs_mode = false;
                }
                i += 2;  // Skip '(' and charset character
                continue;
            }

            // Handle OSC sequences: ESC ] ... BEL or ESC ] ... ST
            if (next == ']') {
                i += 2;
                // Skip until we find BEL (0x07) or ST (ESC \)
                while (i < length) {
                    if (data[i] == 0x07) break;  // BEL
                    if (data[i] == '\033' && i + 1 < length && data[i + 1] == '\\') {
                        i++;  // Skip the backslash too
                        break;
                    }
                    i++;
                }
                continue;
            }

            // Handle other single-character ESC sequences
            if (next == '7' || next == '8' || next == 'M' || next == 'E' ||
                next == 'D' || next == 'H' || next == '=' || next == '>') {
                // ESC 7 = Save cursor, ESC 8 = Restore cursor
                // ESC M = Reverse index, ESC E = Next line
                // ESC D = Index, ESC H = Tab set
                // ESC = = Application keypad, ESC > = Normal keypad
                i++;  // Skip the command character
                continue;
            }

            if (next == '[') {
                // CSI sequence
                i += 2;
                int params[16] = {0};
                int param_count = 0;
                int current_param = 0;
                bool has_param = false;
                bool dec_private_mode = false;

                // Parse parameters
                while (i < length) {
                    c = data[i];
                    if (isdigit(c)) {
                        current_param = current_param * 10 + (c - '0');
                        has_param = true;
                        i++;
                    } else if (c == ';') {
                        if (param_count < 16) {
                            params[param_count++] = current_param;
                        }
                        current_param = 0;
                        has_param = false;
                        i++;
                    } else if (c == '?' || c == '>') {
                        // DEC private mode prefix
                        dec_private_mode = true;
                        i++;
                    } else {
                        // Command character
                        if (has_param && param_count < 16) {
                            params[param_count++] = current_param;
                        }

                        switch (c) {
                            case 'A':  // Cursor up
                                ansi_cursor_up(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'B':  // Cursor down
                                ansi_cursor_down(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'C':  // Cursor forward
                                ansi_cursor_forward(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'D':  // Cursor back
                                ansi_cursor_back(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'H':  // Cursor position
                            case 'f':
                                ansi_set_cursor(term,
                                    (params[0] > 0 ? params[0] - 1 : 0),
                                    (param_count > 1 && params[1] > 0 ? params[1] - 1 : 0));
                                break;
                            case 'J':  // Clear screen
                                ansi_clear_screen(term, params[0]);
                                break;
                            case 'K':  // Clear line
                                ansi_clear_line(term, params[0]);
                                break;
                            case 'm':  // SGR (colors and attributes)
                                if (param_count == 0) {
                                    params[0] = 0;
                                    param_count = 1;
                                }
                                ansi_process_sgr(term, params, param_count);
                                break;
                            case 'h':  // Set mode
                                if (dec_private_mode) {
                                    // DEC private modes (ESC [ ? Ps h)
                                    // Silently ignore mouse tracking, alt screen, etc.
                                    // Common modes: 25=cursor, 47/1047/1049=alt screen,
                                    // 1000/1002/1003=mouse, 2004=bracketed paste
                                    if (param_count > 0 && params[0] == 25) {
                                        term->cursor_visible = true;
                                    }
                                    // Ignore all other DEC private modes
                                } else {
                                    // ANSI modes
                                    // Ignore for now
                                }
                                break;
                            case 'l':  // Reset mode
                                if (dec_private_mode) {
                                    // DEC private modes (ESC [ ? Ps l)
                                    if (param_count > 0 && params[0] == 25) {
                                        term->cursor_visible = false;
                                    }
                                    // Ignore all other DEC private modes
                                } else {
                                    // ANSI modes
                                    // Ignore for now
                                }
                                break;
                            case 'r':  // Set scrolling region (DECSTBM)
                                // Ignore for now - basic scrolling already works
                                break;
                            case 's':  // Save cursor position (ANSI.SYS)
                                // Could implement but not critical
                                break;
                            case 'u':  // Restore cursor position (ANSI.SYS)
                                // Could implement but not critical
                                break;
                            case 'n':  // Device Status Report
                                // Ignore - would require writing back to PTY
                                break;
                            case 'c':  // Device Attributes
                                // Ignore - terminal identification
                                break;
                            case 'g':  // Tab Clear
                                // Ignore - tab handling is basic
                                break;
                            case 'G':  // Cursor Horizontal Absolute
                            case '`':  // Same as G
                                if (param_count > 0 && params[0] > 0) {
                                    term->cursor_x = params[0] - 1;
                                    if (term->cursor_x >= ANSI_BUFFER_COLS)
                                        term->cursor_x = ANSI_BUFFER_COLS - 1;
                                }
                                break;
                            case 'd':  // Cursor Vertical Absolute
                                if (param_count > 0 && params[0] > 0) {
                                    term->cursor_y = params[0] - 1;
                                    if (term->cursor_y >= ANSI_BUFFER_ROWS)
                                        term->cursor_y = ANSI_BUFFER_ROWS - 1;
                                }
                                break;
                            case 'S':  // Scroll up
                            case 'T':  // Scroll down
                                // Ignore - scrolling handled automatically
                                break;
                            case 'X':  // Erase characters
                            case 'P':  // Delete characters
                            case 'L':  // Insert lines
                            case 'M':  // Delete lines
                            case '@':  // Insert characters
                                // Ignore - not critical for most apps
                                break;
                            default:
                                // Unknown CSI sequence - ignore it
                                // This prevents crashes from unrecognized sequences
                                break;
                        }
                        break;
                    }
                }
            } else {
                // Unknown ESC sequence type - skip the ESC and next character
                // This prevents crashes from malformed or unsupported sequences
                i++;  // Skip one more character after ESC
            }
            continue;
        }

        // Handle control characters
        switch (c) {
            case '\n':
                ansi_newline(term);
                break;
            case '\r':
                ansi_carriage_return(term);
                break;
            case '\t':
                // Tab to next 8-column boundary
                term->cursor_x = ((term->cursor_x + 8) / 8) * 8;
                if (term->cursor_x >= ANSI_BUFFER_COLS) {
                    term->cursor_x = 0;
                    ansi_newline(term);
                }
                break;
            case '\b':  // Backspace
                if (term->cursor_x > 0) term->cursor_x--;
                break;
            case 0x0E:  // SO (Shift Out) - Switch to alternate character set
                term->acs_mode = true;
                break;
            case 0x0F:  // SI (Shift In) - Switch back to normal character set
                term->acs_mode = false;
                break;
            default:
                if (c >= 32 && c < 127) {  // Printable ASCII
                    ansi_putchar(term, c);
                } else if ((unsigned char)c >= 128) {
                    // Handle extended ASCII / box-drawing characters
                    // Map common box-drawing characters to ASCII equivalents
                    char replacement = '?';
                    unsigned char uc = (unsigned char)c;
                    bool char_handled = false;

                    // VT100 line drawing characters (0xB0-0xDF range when in ACS mode)
                    // Also handle if programs send these directly
                    switch (uc) {
                        case 0xB0: case 0xB1: replacement = '+'; break;  // corners
                        case 0xB2: case 0xB3: replacement = '-'; break;  // horizontal
                        case 0xB4: case 0xB5: replacement = '+'; break;  // T shapes
                        case 0xB6: case 0xB7: replacement = '+'; break;  // corners
                        case 0xB8: case 0xB9: replacement = '+'; break;  // corners
                        case 0xBA: replacement = '|'; break;  // vertical
                        case 0xBB: case 0xBC: replacement = '+'; break;  // corners
                        case 0xBD: case 0xBE: replacement = '+'; break;  // T shapes
                        case 0xBF: replacement = '+'; break;  // corner
                        case 0xC0: case 0xC1: replacement = '+'; break;  // corners
                        case 0xC2: case 0xC3: replacement = '+'; break;  // T shapes
                        case 0xC4: replacement = '-'; break;  // horizontal
                        case 0xC5: replacement = '+'; break;  // cross
                        case 0xC6: case 0xC7: replacement = '+'; break;  // T shapes
                        case 0xC8: case 0xC9: replacement = '+'; break;  // corners
                        case 0xCA: case 0xCB: replacement = '+'; break;  // T shapes
                        case 0xCC: case 0xCD: replacement = '+'; break;  // T shapes
                        case 0xCE: case 0xCF: replacement = '+'; break;  // T shapes
                        case 0xD0: case 0xD1: replacement = '+'; break;  // T shapes
                        case 0xD2: case 0xD3: replacement = '+'; break;  // corners
                        case 0xD4: case 0xD5: replacement = '+'; break;  // T shapes
                        case 0xD6: case 0xD7: replacement = '+'; break;  // T shapes
                        case 0xD8: case 0xD9: replacement = '+'; break;  // corners
                        case 0xDA: replacement = '+'; break;  // corner
                        default:
                            // For other extended chars, try to display them anyway
                            if (uc >= 32 && uc <= 255) {
                                ansi_putchar(term, c);
                                char_handled = true;
                            }
                            break;
                    }
                    // Only print replacement if we didn't already handle the char
                    if (!char_handled) {
                        ansi_putchar(term, replacement);
                    }
                }
                break;
        }
    }
}

void ansi_get_line(AnsiTerminal* term, int row, char* output, int max_len) {
    if (!term || !output || max_len < 1) {
        if (output && max_len > 0) output[0] = '\0';
        return;
    }

    if (row < 0 || row >= ANSI_BUFFER_ROWS) {
        output[0] = '\0';
        return;
    }

    int len = 0;
    for (int x = 0; x < ANSI_BUFFER_COLS && len < max_len - 1; x++) {
        char c = term->buffer[row][x].character;
        if (c == 0) c = ' ';
        output[len++] = c;
    }

    // Trim trailing spaces
    while (len > 0 && output[len - 1] == ' ') {
        len--;
    }

    output[len] = '\0';
}

void ansi_clear(AnsiTerminal* term) {
    ansi_clear_screen(term, 2);
}

void ansi_get_cursor(AnsiTerminal* term, int* x, int* y) {
    *x = term->cursor_x;
    *y = term->cursor_y;
}

bool ansi_is_dirty(AnsiTerminal* term) {
    return term->dirty;
}

void ansi_mark_clean(AnsiTerminal* term) {
    term->dirty = false;
}
