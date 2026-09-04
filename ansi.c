#include "ansi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>


// Helper: map logical row to physical row (ring buffer)
static inline int logical_to_physical(AnsiTerminal* term, int logical_row) {
    // Handle negative values correctly (C modulo can return negative for negative dividend)
    int result = (term->first_row + logical_row) % ANSI_BUFFER_ROWS;
    if (result < 0) result += ANSI_BUFFER_ROWS;
    return result;
}

int ansi_screen_rows(AnsiTerminal* term) {
    if (!term) return 0;
    if (!term->alt_screen) return ANSI_BUFFER_ROWS;
    int rows = term->term_rows;
    if (rows > ANSI_ALT_ROWS) rows = ANSI_ALT_ROWS;
    if (rows < 1) rows = 1;
    return rows;
}

bool ansi_is_alt_screen(AnsiTerminal* term) {
    return term && term->alt_screen;
}

// Storage for one logical row. The alternate screen is screen-sized and has no
// scrollback, so its rows are indexed directly; the normal screen goes through
// the scrollback ring buffer. Returns NULL for an out-of-range row.
static inline AnsiCell* ansi_row(AnsiTerminal* term, int logical_row) {
    if (term->alt_screen) {
        if (logical_row < 0 || logical_row >= ansi_screen_rows(term)) return NULL;
        return term->alt_buffer[logical_row];
    }
    if (logical_row < 0 || logical_row >= ANSI_BUFFER_ROWS) return NULL;
    int physical = logical_to_physical(term, logical_row);
    if (physical < 0 || physical >= ANSI_BUFFER_ROWS) return NULL;
    return term->buffer[physical];
}

static inline int* ansi_row_len(AnsiTerminal* term, int logical_row) {
    if (term->alt_screen) {
        if (logical_row < 0 || logical_row >= ansi_screen_rows(term)) return NULL;
        return &term->alt_line_len[logical_row];
    }
    if (logical_row < 0 || logical_row >= ANSI_BUFFER_ROWS) return NULL;
    int physical = logical_to_physical(term, logical_row);
    if (physical < 0 || physical >= ANSI_BUFFER_ROWS) return NULL;
    return &term->line_len[physical];
}

// Colours as they should be stored, accounting for reverse video (SGR 7).
static inline void ansi_effective_colors(AnsiTerminal* term,
                                         unsigned char* fg, unsigned char* bg) {
    if (term->current_reverse) {
        *fg = term->current_bg;
        *bg = term->current_fg;
    } else {
        *fg = term->current_fg;
        *bg = term->current_bg;
    }
}

// Blank a row using the current background, the way an erase would.
static void ansi_blank_row(AnsiTerminal* term, int logical_row) {
    AnsiCell* row = ansi_row(term, logical_row);
    int* len = ansi_row_len(term, logical_row);
    if (!row || !len) return;
    unsigned char fg, bg;
    ansi_effective_colors(term, &fg, &bg);
    for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
        row[x].character = ' ';
        row[x].fg_color = fg;
        row[x].bg_color = bg;
        row[x].bold = false;
        row[x].underline = false;
    }
    *len = 0;
}

// Move the scrolling region up (content rises, a blank line enters at the
// bottom) or down (content falls, a blank line enters at the top).
static void ansi_scroll_region(AnsiTerminal* term, bool up) {
    int top = term->scroll_top;
    int bottom = term->scroll_bottom;
    int rows = ansi_screen_rows(term);
    if (top < 0) top = 0;
    if (bottom >= rows) bottom = rows - 1;
    if (top >= bottom) return;

    if (up) {
        for (int y = top; y < bottom; y++) {
            AnsiCell* dst = ansi_row(term, y);
            AnsiCell* src = ansi_row(term, y + 1);
            int* dl = ansi_row_len(term, y);
            int* sl = ansi_row_len(term, y + 1);
            if (dst && src) memcpy(dst, src, sizeof(AnsiCell) * ANSI_BUFFER_COLS);
            if (dl && sl) *dl = *sl;
        }
        ansi_blank_row(term, bottom);
    } else {
        for (int y = bottom; y > top; y--) {
            AnsiCell* dst = ansi_row(term, y);
            AnsiCell* src = ansi_row(term, y - 1);
            int* dl = ansi_row_len(term, y);
            int* sl = ansi_row_len(term, y - 1);
            if (dst && src) memcpy(dst, src, sizeof(AnsiCell) * ANSI_BUFFER_COLS);
            if (dl && sl) *dl = *sl;
        }
        ansi_blank_row(term, top);
    }
    term->dirty = true;
}

static void ansi_reset_scroll_region(AnsiTerminal* term) {
    term->scroll_top = 0;
    term->scroll_bottom = ansi_screen_rows(term) - 1;
}

static void ansi_enter_alt_screen(AnsiTerminal* term, bool save_cursor) {
    if (term->alt_screen) return;
    if (save_cursor) {
        term->alt_saved_cursor_x = term->cursor_x;
        term->alt_saved_cursor_y = term->cursor_y;
    }
    term->alt_screen = true;
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->pending_wrap = false;
    ansi_reset_scroll_region(term);
    for (int y = 0; y < ansi_screen_rows(term); y++) {
        ansi_blank_row(term, y);
    }
    term->dirty = true;
}

static void ansi_leave_alt_screen(AnsiTerminal* term, bool restore_cursor) {
    if (!term->alt_screen) return;
    term->alt_screen = false;
    term->pending_wrap = false;
    ansi_reset_scroll_region(term);
    if (restore_cursor) {
        term->cursor_x = term->alt_saved_cursor_x;
        term->cursor_y = term->alt_saved_cursor_y;
    }
    if (term->cursor_y < 0) term->cursor_y = 0;
    if (term->cursor_y >= ANSI_BUFFER_ROWS) term->cursor_y = ANSI_BUFFER_ROWS - 1;
    if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
    term->dirty = true;
}

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
    term->first_row = 0;  // Ring buffer starts at physical row 0

    // Until the PTY is sized, assume the full buffer width so nothing is lost.
    term->term_cols = ANSI_BUFFER_COLS;
    term->term_rows = ANSI_BUFFER_ROWS;
    term->pending_wrap = false;

    // Initialize parser state
    term->escape_buffer_len = 0;
    term->in_escape = false;

    // Clear buffer and line lengths
    for (int y = 0; y < ANSI_BUFFER_ROWS; y++) {
        term->line_len[y] = 0;
        for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
            term->buffer[y][x].character = ' ';
            term->buffer[y][x].fg_color = 7;
            term->buffer[y][x].bg_color = 0;
            term->buffer[y][x].bold = false;
            term->buffer[y][x].underline = false;
        }
    }

    for (int y = 0; y < ANSI_ALT_ROWS; y++) {
        term->alt_line_len[y] = 0;
        for (int x = 0; x < ANSI_BUFFER_COLS; x++) {
            term->alt_buffer[y][x].character = ' ';
            term->alt_buffer[y][x].fg_color = 7;
            term->alt_buffer[y][x].bg_color = 0;
            term->alt_buffer[y][x].bold = false;
            term->alt_buffer[y][x].underline = false;
        }
    }
}

static int is_escape_complete(const char* data, int len, int start);

// Offset of a trailing incomplete escape sequence, or -1 if the data does not
// end in one. Only the tail is scanned: a sequence longer than escape_buffer
// could not be stashed anyway.
static int find_trailing_partial_escape(const char* data, int length, int max_stash) {
    int limit = length > max_stash ? length - max_stash : 0;
    for (int i = length - 1; i >= limit; i--) {
        if (data[i] != '\033' && data[i] != '\x1b') continue;
        // This is the last ESC in the buffer, so nothing after it can start
        // another sequence: its state decides the answer either way.
        return is_escape_complete(data, length, i) == -1 ? i : -1;
    }
    return -1;
}

void ansi_set_size(AnsiTerminal* term, int cols, int rows) {
    if (!term) return;

    if (cols < 1) cols = 1;
    if (cols > ANSI_BUFFER_COLS) cols = ANSI_BUFFER_COLS;
    if (rows < 1) rows = 1;
    if (rows > ANSI_BUFFER_ROWS) rows = ANSI_BUFFER_ROWS;

    if (cols == term->term_cols && rows == term->term_rows) return;

    term->term_cols = cols;
    term->term_rows = rows;

    // A narrower screen can leave the cursor past the new right edge.
    if (term->cursor_x >= cols) term->cursor_x = cols - 1;
    if (term->alt_screen) {
        int screen_rows = ansi_screen_rows(term);
        if (term->cursor_y >= screen_rows) term->cursor_y = screen_rows - 1;
        if (term->scroll_bottom >= screen_rows || term->scroll_bottom <= term->scroll_top) {
            ansi_reset_scroll_region(term);
        }
    }
    term->pending_wrap = false;
    term->dirty = true;
}

// Check if we have a complete escape sequence
// Returns: -1 = need more data, 0 = invalid/abandon, >0 = complete length
static int is_escape_complete(const char* data, int len, int start) {
    if (start >= len) return -1;
    if (data[start] != '\033' && data[start] != '\x1b') return 0;

    if (start + 1 >= len) return -1;  // Need at least one more character

    char next = data[start + 1];

    // Single-character ESC sequences
    if (next == '7' || next == '8' || next == 'M' || next == 'E' ||
        next == 'D' || next == 'H' || next == '=' || next == '>') {
        return 2;  // ESC + command
    }

    // Character set selection: ESC ( X or ESC ) X
    if ((next == '(' || next == ')') && start + 2 < len) {
        return 3;  // ESC ( X
    } else if (next == '(' || next == ')') {
        return -1;  // Need one more char
    }

    // OSC sequence: ESC ] ... BEL or ESC ] ... ST (ESC \)
    if (next == ']') {
        for (int i = start + 2; i < len; i++) {
            if (data[i] == 0x07) return i - start + 1;  // Found BEL
            if (i + 1 < len && data[i] == '\033' && data[i+1] == '\\') {
                return i - start + 2;  // Found ST
            }
        }
        return -1;  // Incomplete OSC
    }

    // CSI sequence: ESC [ ... final_byte
    if (next == '[') {
        for (int i = start + 2; i < len; i++) {
            char c = data[i];
            // Private marker
            if (i == start + 2 && (c == '?' || c == '>')) {
                continue;
            }
            // Parameter bytes: digits, semicolon
            if ((c >= '0' && c <= '9') || c == ';') continue;
            // Final byte: letter or @ to ~
            if ((c >= '@' && c <= '~') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                return i - start + 1;  // Complete!
            }
            // Invalid character - abandon sequence
            return 0;
        }
        return -1;  // Incomplete CSI
    }

    // Unknown ESC sequence - treat as 2-char sequence
    return 2;
}

// Forward declarations
static void ansi_newline(AnsiTerminal* term);

// Byte length of the UTF-8 sequence starting with `lead`, or 0 if `lead` is
// not a valid start byte (a stray continuation byte, or an overlong form).
static int utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return lead >= 0xC2 ? 2 : 0;  // 0xC0/0xC1 are overlong
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return lead <= 0xF4 ? 4 : 0;
    return 0;
}

// Fold a Unicode codepoint onto the ASCII range the font can actually draw.
// The point is not fidelity - it is that one character occupies one cell, so
// box drawing and tables keep their columns.
static char codepoint_to_ascii(unsigned int cp) {
    if (cp < 128) return (char)cp;

    switch (cp) {
        case 0x00A0: return ' ';   // no-break space
        case 0x00B0: return 'o';   // degree
        case 0x00B1: return '+';   // plus-minus
        case 0x00B7: return '.';   // middle dot
        case 0x00D7: return 'x';   // multiplication
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: return '-';   // dashes
        case 0x2018: case 0x2019: return '\'';               // curly single quotes
        case 0x201C: case 0x201D: return '"';                // curly double quotes
        case 0x2022: return 'o';   // bullet
        case 0x2026: return '.';   // ellipsis
        case 0x2190: return '<';   // arrows
        case 0x2191: return '^';
        case 0x2192: return '>';
        case 0x2193: return 'v';
        case 0x2713: case 0x2714: return 'v';   // check marks
        case 0x2717: case 0x2718: return 'x';   // ballot X
        default: break;
    }

    // Box drawing (U+2500-U+257F): split into horizontal, vertical and joints.
    if (cp >= 0x2500 && cp <= 0x257F) {
        switch (cp) {
            case 0x2500: case 0x2501: case 0x2504: case 0x2505:
            case 0x2508: case 0x2509: case 0x254C: case 0x254D:
            case 0x2550: case 0x2574: case 0x2576: case 0x257C:
            case 0x257E:
                return '-';
            case 0x2502: case 0x2503: case 0x2506: case 0x2507:
            case 0x250A: case 0x250B: case 0x254E: case 0x254F:
            case 0x2551: case 0x2575: case 0x2577: case 0x257D:
            case 0x257F:
                return '|';
            case 0x2571: return '/';
            case 0x2572: return '\\';
            default:
                return '+';   // corners, tees and crosses
        }
    }

    // Block elements, shading and geometric shapes.
    if (cp >= 0x2580 && cp <= 0x25FF) return '#';

    return '?';
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
    int screen_rows = ansi_screen_rows(term);
    if (term->cursor_y < 0) term->cursor_y = 0;
    if (term->cursor_y >= screen_rows) {
        term->cursor_y = screen_rows - 1;
    }
    if (term->cursor_x < 0) term->cursor_x = 0;
    if (term->cursor_x >= term->term_cols) {
        term->cursor_x = term->term_cols - 1;
    }

    // A glyph was already written into the last column; wrap now, before
    // placing this one.
    if (term->pending_wrap) {
        ansi_newline(term);
    }

    // If in ACS mode, convert character
    if (term->acs_mode) {
        c = acs_to_ascii(c);
    }

    AnsiCell* row = ansi_row(term, term->cursor_y);
    int* row_len = ansi_row_len(term, term->cursor_y);
    if (row && row_len && term->cursor_x >= 0 && term->cursor_x < ANSI_BUFFER_COLS) {
        unsigned char fg, bg;
        ansi_effective_colors(term, &fg, &bg);
        row[term->cursor_x].character = c;
        row[term->cursor_x].fg_color = fg;
        row[term->cursor_x].bg_color = bg;
        row[term->cursor_x].bold = term->current_bold;
        row[term->cursor_x].underline = term->current_underline;

        // Track line length for optimization
        if (term->cursor_x + 1 > *row_len) {
            *row_len = term->cursor_x + 1;
        }
    }

    term->cursor_x++;
    if (term->cursor_x >= term->term_cols) {
        // Park in the last column and defer the wrap until the next glyph.
        term->cursor_x = term->term_cols - 1;
        term->pending_wrap = true;
    }
    term->dirty = true;
}

static void ansi_newline(AnsiTerminal* term) {
    if (!term) return;  // Safety check

    term->cursor_x = 0;
    term->cursor_y++;
    term->pending_wrap = false;

    // The alternate screen is a fixed-size screen: it scrolls its region in
    // place rather than pushing lines into scrollback.
    if (term->alt_screen) {
        int rows = ansi_screen_rows(term);
        int bottom = term->scroll_bottom;
        if (bottom >= rows) bottom = rows - 1;

        if (term->cursor_y > bottom) {
            term->cursor_y = bottom;
            ansi_scroll_region(term, true);
        }
        if (term->cursor_y < 0) term->cursor_y = 0;
        if (term->cursor_y >= rows) term->cursor_y = rows - 1;
        term->dirty = true;
        return;
    }

    // Scroll if needed - OPTIMIZED: use ring buffer instead of shifting entire buffer
    if (term->cursor_y >= ANSI_BUFFER_ROWS) {
        // Clamp first_row to valid range before incrementing (defensive)
        if (term->first_row < 0) term->first_row = 0;
        if (term->first_row >= ANSI_BUFFER_ROWS) term->first_row = 0;

        // Instead of copying all rows, just advance the ring buffer pointer
        term->first_row = (term->first_row + 1) % ANSI_BUFFER_ROWS;
        term->cursor_y = ANSI_BUFFER_ROWS - 1;

        // Clear only the new last line (which is now the "oldest" physical row)
        ansi_blank_row(term, term->cursor_y);
    }

    // Extra safety: ensure cursor_y never exceeds buffer bounds
    if (term->cursor_y >= ANSI_BUFFER_ROWS) {
        term->cursor_y = ANSI_BUFFER_ROWS - 1;
    }
    if (term->cursor_y < 0) {
        term->cursor_y = 0;
    }

    term->dirty = true;
}

static void ansi_carriage_return(AnsiTerminal* term) {
    term->cursor_x = 0;
    term->pending_wrap = false;
}

static void ansi_clear_screen(AnsiTerminal* term, int mode) {
    if (!term) return;

    // Clamp cursor to valid range
    int rows = ansi_screen_rows(term);
    if (term->cursor_y < 0) term->cursor_y = 0;
    if (term->cursor_y >= rows) term->cursor_y = rows - 1;
    if (term->cursor_x < 0) term->cursor_x = 0;
    if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
    term->pending_wrap = false;

    switch (mode) {
        case 0:  // Clear from cursor to end
            {
                AnsiCell* row = ansi_row(term, term->cursor_y);
                int* len = ansi_row_len(term, term->cursor_y);
                if (row && len) {
                    for (int x = term->cursor_x; x < ANSI_BUFFER_COLS; x++) {
                        row[x].character = ' ';
                        ansi_effective_colors(term, &row[x].fg_color, &row[x].bg_color);
                    }
                    *len = term->cursor_x;
                }
                for (int y = term->cursor_y + 1; y < rows; y++) {
                    ansi_blank_row(term, y);
                }
            }
            break;
        case 1:  // Clear from start to cursor
            {
                for (int y = 0; y < term->cursor_y; y++) {
                    ansi_blank_row(term, y);
                }
                AnsiCell* row = ansi_row(term, term->cursor_y);
                int* len = ansi_row_len(term, term->cursor_y);
                if (row && len) {
                    for (int x = 0; x <= term->cursor_x && x < ANSI_BUFFER_COLS; x++) {
                        row[x].character = ' ';
                        ansi_effective_colors(term, &row[x].fg_color, &row[x].bg_color);
                    }
                    if (*len <= term->cursor_x + 1) {
                        *len = term->cursor_x + 1;
                    }
                }
            }
            break;
        case 2:  // Clear entire screen
        case 3:  // Clear entire screen and scrollback
            for (int y = 0; y < rows; y++) {
                ansi_blank_row(term, y);
            }
            term->cursor_x = 0;
            term->cursor_y = 0;
            if (!term->alt_screen) term->first_row = 0;  // Reset ring buffer
            break;
    }
    term->dirty = true;
}

static void ansi_clear_line(AnsiTerminal* term, int mode) {
    if (!term) return;

    // Clamp cursor to valid range
    int rows = ansi_screen_rows(term);
    if (term->cursor_y < 0) term->cursor_y = 0;
    if (term->cursor_y >= rows) term->cursor_y = rows - 1;
    if (term->cursor_x < 0) term->cursor_x = 0;
    if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
    term->pending_wrap = false;

    AnsiCell* row = ansi_row(term, term->cursor_y);
    int* len = ansi_row_len(term, term->cursor_y);
    if (!row || !len) return;

    switch (mode) {
        case 0:  // Clear from cursor to end of line
            for (int x = term->cursor_x; x < ANSI_BUFFER_COLS; x++) {
                row[x].character = ' ';
                ansi_effective_colors(term, &row[x].fg_color, &row[x].bg_color);
            }
            *len = term->cursor_x;
            break;
        case 1:  // Clear from start of line to cursor
            for (int x = 0; x <= term->cursor_x && x < term->term_cols; x++) {
                row[x].character = ' ';
                ansi_effective_colors(term, &row[x].fg_color, &row[x].bg_color);
            }
            // line_len might extend past cursor, leave it
            break;
        case 2:  // Clear entire line
            ansi_blank_row(term, term->cursor_y);
            break;
    }
    term->dirty = true;
}

static void ansi_set_cursor(AnsiTerminal* term, int row, int col) {
    int rows = ansi_screen_rows(term);
    term->cursor_y = row < 0 ? 0 : (row >= rows ? rows - 1 : row);
    term->cursor_x = col < 0 ? 0 : (col >= term->term_cols ? term->term_cols - 1 : col);
    term->pending_wrap = false;
}

static void ansi_cursor_up(AnsiTerminal* term, int n) {
    term->cursor_y -= n;
    if (term->cursor_y < 0) term->cursor_y = 0;
    term->pending_wrap = false;
}

static void ansi_cursor_down(AnsiTerminal* term, int n) {
    int rows = ansi_screen_rows(term);
    term->cursor_y += n;
    if (term->cursor_y >= rows) term->cursor_y = rows - 1;
    term->pending_wrap = false;
}

static void ansi_cursor_forward(AnsiTerminal* term, int n) {
    term->cursor_x += n;
    if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
    term->pending_wrap = false;
}

static void ansi_cursor_back(AnsiTerminal* term, int n) {
    term->cursor_x -= n;
    if (term->cursor_x < 0) term->cursor_x = 0;
    term->pending_wrap = false;
}

static void ansi_copy_row(AnsiTerminal* term, int dst_y, int src_y) {
    AnsiCell* dst = ansi_row(term, dst_y);
    AnsiCell* src = ansi_row(term, src_y);
    int* dl = ansi_row_len(term, dst_y);
    int* sl = ansi_row_len(term, src_y);
    if (dst && src) memcpy(dst, src, sizeof(AnsiCell) * ANSI_BUFFER_COLS);
    if (dl && sl) *dl = *sl;
}

// LF without the CR: move down one line, scrolling at the bottom.
static void ansi_index(AnsiTerminal* term) {
    int saved_x = term->cursor_x;
    ansi_newline(term);
    term->cursor_x = saved_x;
    if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
}

// RI: move up one line, scrolling the region down at the top.
static void ansi_reverse_index(AnsiTerminal* term) {
    if (term->alt_screen && term->cursor_y <= term->scroll_top) {
        ansi_scroll_region(term, false);
    } else if (term->cursor_y > 0) {
        term->cursor_y--;
    }
    term->pending_wrap = false;
    term->dirty = true;
}

// IL / DL. Only meaningful on the alternate screen, which is the only place
// with a fixed screen to shift lines within.
static void ansi_insert_lines(AnsiTerminal* term, int n) {
    if (!term->alt_screen) return;
    int rows = ansi_screen_rows(term);
    int bottom = term->scroll_bottom < rows ? term->scroll_bottom : rows - 1;
    if (term->cursor_y < term->scroll_top || term->cursor_y > bottom) return;
    if (n < 1) n = 1;

    for (int k = 0; k < n; k++) {
        for (int y = bottom; y > term->cursor_y; y--) {
            ansi_copy_row(term, y, y - 1);
        }
        ansi_blank_row(term, term->cursor_y);
    }
    term->pending_wrap = false;
    term->dirty = true;
}

static void ansi_delete_lines(AnsiTerminal* term, int n) {
    if (!term->alt_screen) return;
    int rows = ansi_screen_rows(term);
    int bottom = term->scroll_bottom < rows ? term->scroll_bottom : rows - 1;
    if (term->cursor_y < term->scroll_top || term->cursor_y > bottom) return;
    if (n < 1) n = 1;

    for (int k = 0; k < n; k++) {
        for (int y = term->cursor_y; y < bottom; y++) {
            ansi_copy_row(term, y, y + 1);
        }
        ansi_blank_row(term, bottom);
    }
    term->pending_wrap = false;
    term->dirty = true;
}

// ICH / DCH / ECH, all within the cursor's row.
static void ansi_edit_chars(AnsiTerminal* term, int n, char op) {
    AnsiCell* row = ansi_row(term, term->cursor_y);
    int* len = ansi_row_len(term, term->cursor_y);
    if (!row || !len) return;

    int width = term->term_cols;
    int x = term->cursor_x;
    if (n < 1) n = 1;
    if (n > width - x) n = width - x;
    if (n <= 0) return;

    AnsiCell blank;
    blank.character = ' ';
    ansi_effective_colors(term, &blank.fg_color, &blank.bg_color);
    blank.bold = false;
    blank.underline = false;

    if (op == '@') {          // insert: push the tail right
        for (int i = width - 1; i >= x + n; i--) row[i] = row[i - n];
        for (int i = x; i < x + n; i++) row[i] = blank;
        if (*len + n > width) *len = width; else if (*len > x) *len += n;
    } else if (op == 'P') {   // delete: pull the tail left
        for (int i = x; i < width - n; i++) row[i] = row[i + n];
        for (int i = width - n; i < width; i++) row[i] = blank;
        *len -= n;
        if (*len < x) *len = x;
    } else {                  // 'X' erase in place
        for (int i = x; i < x + n; i++) row[i] = blank;
    }

    term->pending_wrap = false;
    term->dirty = true;
}

// Map a 256-colour index onto the 16 colours a cell can store.
static unsigned char ansi_256_to_16(int idx) {
    if (idx < 0) return ANSI_DEFAULT_FG;
    if (idx < 16) return (unsigned char)idx;

    if (idx < 232) {                        // 6x6x6 colour cube
        int c = idx - 16;
        int r = (c / 36) % 6, g = (c / 6) % 6, b = c % 6;
        unsigned char base = (unsigned char)((r >= 3 ? 1 : 0) |
                                             (g >= 3 ? 2 : 0) |
                                             (b >= 3 ? 4 : 0));
        return (r > 3 || g > 3 || b > 3) ? (unsigned char)(base + 8) : base;
    }

    int level = idx - 232;                  // 24-step greyscale ramp
    if (level < 6) return 0;                // black
    if (level < 12) return 8;               // dark grey
    if (level < 18) return 7;               // light grey
    return 15;                              // white
}

static unsigned char ansi_rgb_to_16(int r, int g, int b) {
    unsigned char base = (unsigned char)((r > 85 ? 1 : 0) |
                                         (g > 85 ? 2 : 0) |
                                         (b > 85 ? 4 : 0));
    return (r > 170 || g > 170 || b > 170) ? (unsigned char)(base + 8) : base;
}

static void ansi_process_sgr(AnsiTerminal* term, int* params, int count) {
    // "ESC [ m" carries no parameters and means "ESC [ 0 m" - reset everything.
    // The caller zero-initialises params, so treating it as one zero parameter
    // is exactly right. Dropping it left attributes latched on forever.
    if (count < 1) count = 1;

    for (int i = 0; i < count; i++) {
        int param = params[i];

        switch (param) {
            case 0:  // Reset
                term->current_fg = ANSI_DEFAULT_FG;
                term->current_bg = ANSI_DEFAULT_BG;
                term->current_bold = false;
                term->current_underline = false;
                term->current_reverse = false;
                break;
            case 1:  // Bold
                term->current_bold = true;
                break;
            case 4:  // Underline
                term->current_underline = true;
                break;
            case 7:  // Reverse video
                term->current_reverse = true;
                break;
            case 21:  // Doubly underlined, widely treated as bold off
            case 22:  // Normal intensity
                term->current_bold = false;
                break;
            case 24:  // Not underlined
                term->current_underline = false;
                break;
            case 27:  // Not reversed
                term->current_reverse = false;
                break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:  // Foreground color
                term->current_fg = param - 30;
                break;
            case 38:  // Extended foreground: 38;5;N or 38;2;R;G;B
                if (i + 2 < count && params[i + 1] == 5) {
                    term->current_fg = ansi_256_to_16(params[i + 2]);
                    i += 2;
                } else if (i + 4 < count && params[i + 1] == 2) {
                    term->current_fg = ansi_rgb_to_16(params[i + 2], params[i + 3], params[i + 4]);
                    i += 4;
                }
                break;
            case 39:  // Default foreground
                term->current_fg = ANSI_DEFAULT_FG;
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:  // Background color
                term->current_bg = param - 40;
                break;
            case 48:  // Extended background
                if (i + 2 < count && params[i + 1] == 5) {
                    term->current_bg = ansi_256_to_16(params[i + 2]);
                    i += 2;
                } else if (i + 4 < count && params[i + 1] == 2) {
                    term->current_bg = ansi_rgb_to_16(params[i + 2], params[i + 3], params[i + 4]);
                    i += 4;
                }
                break;
            case 49:  // Default background
                term->current_bg = ANSI_DEFAULT_BG;
                break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:  // Bright foreground
                term->current_fg = param - 90 + 8;
                break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:  // Bright background
                term->current_bg = param - 100 + 8;
                break;
            default:
                break;
        }
    }
}

// FIXED: This ANSI parser now handles split escape sequences across buffer boundaries
// Using state machine with escape_buffer to accumulate partial sequences
void ansi_process_output(AnsiTerminal* term, const char* data, int length) {
    // Safety checks
    if (!term || !data || length <= 0) return;

    // Declare at function scope for use in both fast and slow paths
    const char* process_data;
    static char combined[8192];  // Static to avoid stack allocation

    // A UTF-8 character split across two reads left its leading bytes behind;
    // put them back in front of this chunk before anything else looks at it.
    static char stitched[sizeof(combined) + 4];
    if (term->utf8_pending_len > 0) {
        int pending = term->utf8_pending_len;
        term->utf8_pending_len = 0;
        if (length > (int)sizeof(stitched) - pending) {
            length = (int)sizeof(stitched) - pending;
        }
        memcpy(stitched, term->utf8_pending, pending);
        memcpy(stitched + pending, data, length);
        data = stitched;
        length += pending;
    }

    // OPTIMIZATION: Fast path - if no buffered data, process in place.
    if (term->escape_buffer_len == 0) {
        // Stash a trailing incomplete escape for the next read. This has to run
        // for every size of read: it used to be skipped for anything 32 bytes
        // or larger, so a sequence straddling a chunk boundary was dropped and
        // its tail printed as literal text - which also left attributes such as
        // reverse video latched on when it was the reset that got eaten.
        int partial = find_trailing_partial_escape(data, length,
                                                   (int)sizeof(term->escape_buffer) - 1);
        if (partial >= 0) {
            term->escape_buffer_len = length - partial;
            memcpy(term->escape_buffer, data + partial, term->escape_buffer_len);
            length = partial;
        }

        if (length <= 0) return;
        process_data = data;
        goto process_loop;  // Skip the slow combining path
    }

    // Slow path: need to merge buffered data with new data
    int combined_len = 0;

    // Prepend any buffered escape sequence from previous call
    if (term->escape_buffer_len > 0 && term->escape_buffer_len < (int)sizeof(combined)) {
        memcpy(combined, term->escape_buffer, term->escape_buffer_len);
        combined_len = term->escape_buffer_len;
        term->escape_buffer_len = 0;  // Reset buffer
    }

    // Append new data
    int copy_len = length;
    if (combined_len + copy_len > (int)sizeof(combined)) {
        copy_len = (int)sizeof(combined) - combined_len;
    }
    if (copy_len > 0) {
        memcpy(combined + combined_len, data, copy_len);
        combined_len += copy_len;
    }

    // Check if we end on an incomplete escape sequence
    int process_len = combined_len;
    int partial = find_trailing_partial_escape(combined, combined_len,
                                               (int)sizeof(term->escape_buffer) - 1);
    if (partial >= 0) {
        term->escape_buffer_len = combined_len - partial;
        memcpy(term->escape_buffer, combined + partial, term->escape_buffer_len);
        process_len = partial;
    }

    // Process the combined data
    process_data = combined;
    length = process_len;

process_loop:

    for (int i = 0; i < length; i++) {
        char c = process_data[i];

        // Handle escape sequences
        if (c == '\033' || c == '\x1b') {
            // Need at least one more character to determine sequence type
            if (i + 1 >= length) {
                // This should not happen if our incomplete detection works,
                // but as a safety measure, skip this ESC
                break;  // Stop processing here
            }

            char next = process_data[i + 1];

            // Handle character set selection: ESC ( X or ESC ) X
            if ((next == '(' || next == ')') && i + 2 < length) {
                char charset = process_data[i + 2];
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
                    if (process_data[i] == 0x07) break;  // BEL
                    if (process_data[i] == '\033' && i + 1 < length && process_data[i + 1] == '\\') {
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
                switch (next) {
                    case '7':  // DECSC - save cursor
                        term->saved_cursor_x = term->cursor_x;
                        term->saved_cursor_y = term->cursor_y;
                        break;
                    case '8':  // DECRC - restore cursor
                        ansi_set_cursor(term, term->saved_cursor_y, term->saved_cursor_x);
                        break;
                    case 'M':  // RI - reverse index
                        ansi_reverse_index(term);
                        break;
                    case 'D':  // IND - index (line feed, column kept)
                        ansi_index(term);
                        break;
                    case 'E':  // NEL - next line (CR + LF)
                        ansi_newline(term);
                        break;
                    default:
                        // ESC H (tab set), ESC = / ESC > (keypad mode): ignored
                        break;
                }
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
                    c = process_data[i];
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
                                // "CSI ? ... m" is a private sequence, not an
                                // SGR. vim sends ESC[?4m, and reading that as
                                // plain SGR 4 latched underline on for good.
                                if (dec_private_mode) break;
                                if (param_count == 0) {
                                    params[0] = 0;
                                    param_count = 1;
                                }
                                ansi_process_sgr(term, params, param_count);
                                break;
                            case 'h':  // Set mode
                            case 'l': {
                                bool set = (c == 'h');
                                if (!dec_private_mode) break;  // ANSI modes: ignored

                                // DEC private modes. 1000/1002/1003 (mouse) and
                                // 2004 (bracketed paste) are still ignored.
                                for (int pi = 0; pi < param_count; pi++) {
                                    switch (params[pi]) {
                                        case 25:    // DECTCEM - cursor visibility
                                            term->cursor_visible = set;
                                            break;
                                        case 47:
                                        case 1047:  // alt screen, cursor not saved
                                            if (set) ansi_enter_alt_screen(term, false);
                                            else     ansi_leave_alt_screen(term, false);
                                            break;
                                        case 1049:  // alt screen + save/restore cursor
                                            if (set) ansi_enter_alt_screen(term, true);
                                            else     ansi_leave_alt_screen(term, true);
                                            break;
                                        default:
                                            break;
                                    }
                                }
                                break;
                            }
                            case 'r': {  // DECSTBM - set scrolling region
                                // Only the alternate screen has a fixed screen to
                                // scroll within; the normal screen always scrolls
                                // into scrollback.
                                if (!term->alt_screen) break;
                                int screen_rows = ansi_screen_rows(term);
                                int top = (param_count > 0 && params[0] > 0) ? params[0] - 1 : 0;
                                int bottom = (param_count > 1 && params[1] > 0)
                                                ? params[1] - 1 : screen_rows - 1;
                                if (top < 0) top = 0;
                                if (bottom >= screen_rows) bottom = screen_rows - 1;

                                if (top < bottom) {
                                    term->scroll_top = top;
                                    term->scroll_bottom = bottom;
                                } else {
                                    ansi_reset_scroll_region(term);
                                }
                                // DECSTBM homes the cursor.
                                term->cursor_x = 0;
                                term->cursor_y = 0;
                                term->pending_wrap = false;
                                break;
                            }
                            case 's':  // Save cursor position (ANSI.SYS)
                                term->saved_cursor_x = term->cursor_x;
                                term->saved_cursor_y = term->cursor_y;
                                break;
                            case 'u':  // Restore cursor position (ANSI.SYS)
                                ansi_set_cursor(term, term->saved_cursor_y, term->saved_cursor_x);
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
                                    if (term->cursor_x >= term->term_cols)
                                        term->cursor_x = term->term_cols - 1;
                                    term->pending_wrap = false;
                                }
                                break;
                            case 'd':  // Cursor Vertical Absolute
                                if (param_count > 0 && params[0] > 0) {
                                    term->cursor_y = params[0] - 1;
                                    if (term->cursor_y >= ANSI_BUFFER_ROWS)
                                        term->cursor_y = ANSI_BUFFER_ROWS - 1;
                                    term->pending_wrap = false;
                                }
                                break;
                            case 'S':    // SU - scroll region up
                            case 'T': {  // SD - scroll region down
                                if (!term->alt_screen) break;
                                int n = params[0] > 0 ? params[0] : 1;
                                for (int k = 0; k < n; k++) {
                                    ansi_scroll_region(term, c == 'S');
                                }
                                break;
                            }
                            case 'L':  // IL - insert lines
                                ansi_insert_lines(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'M':  // DL - delete lines
                                ansi_delete_lines(term, params[0] > 0 ? params[0] : 1);
                                break;
                            case 'X':  // ECH - erase characters
                            case 'P':  // DCH - delete characters
                            case '@':  // ICH - insert characters
                                ansi_edit_chars(term, params[0] > 0 ? params[0] : 1, c);
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
                // Safety: clamp cursor_x before calculation
                if (term->cursor_x < 0) term->cursor_x = 0;
                if (term->cursor_x >= term->term_cols) term->cursor_x = term->term_cols - 1;
                term->pending_wrap = false;

                term->cursor_x = ((term->cursor_x + 8) / 8) * 8;
                if (term->cursor_x >= term->term_cols) {
                    // A tab never wraps: it stops at the last column.
                    term->cursor_x = term->term_cols - 1;
                }
                break;
            case '\b':  // Backspace
                if (term->cursor_x > 0) term->cursor_x--;
                term->pending_wrap = false;
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
                    // Decode one whole UTF-8 character. The font is ASCII-only
                    // so the result is folded to a stand-in glyph, but decoding
                    // still matters: treating the bytes individually turned one
                    // character into two or three cells and shifted every
                    // column after it.
                    int seq_len = utf8_sequence_length((unsigned char)c);

                    if (seq_len == 0) {
                        ansi_putchar(term, '?');   // stray continuation or invalid lead
                    } else if (i + seq_len > length) {
                        // Truncated by the end of this read - keep the bytes
                        // and pick up where we left off next time.
                        int rem = length - i;
                        if (rem > 0 && rem <= (int)sizeof(term->utf8_pending)) {
                            memcpy(term->utf8_pending, process_data + i, rem);
                            term->utf8_pending_len = rem;
                        }
                        i = length;   // the loop's i++ then ends it
                    } else {
                        static const unsigned int lead_mask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
                        unsigned int cp = (unsigned char)c & lead_mask[seq_len];
                        bool valid = true;

                        for (int k = 1; k < seq_len; k++) {
                            unsigned char cont = (unsigned char)process_data[i + k];
                            if ((cont & 0xC0) != 0x80) { valid = false; break; }
                            cp = (cp << 6) | (cont & 0x3F);
                        }

                        if (valid) {
                            ansi_putchar(term, codepoint_to_ascii(cp));
                            i += seq_len - 1;
                        } else {
                            // Malformed: consume just the lead byte and resync.
                            ansi_putchar(term, '?');
                        }
                    }
                }
                break;
        }
    }
}

int ansi_get_line_cells(AnsiTerminal* term, int row, AnsiCell* out, int max_cells) {
    if (!term || !out || max_cells < 1) return 0;
    if (row < 0 || row >= ansi_screen_rows(term)) return 0;

    AnsiCell* src = ansi_row(term, row);
    int* src_len = ansi_row_len(term, row);
    if (!src || !src_len) return 0;

    // OPTIMIZATION: Only scan up to the tracked line length or max_cells, whichever is smaller
    // BUT: Always scan at least to the cursor position if cursor is on this row
    int scan_limit = *src_len;

    // If cursor is on this row and visible, ensure we scan at least to cursor position + 1
    if (term->cursor_visible && term->cursor_y == row) {
        if (term->cursor_x + 1 > scan_limit) {
            scan_limit = term->cursor_x + 1;
        }
    }

    if (scan_limit > max_cells) scan_limit = max_cells;
    // Never hand back cells past the wrap column - anything out there is stale
    // content left over from a wider window.
    if (scan_limit > term->term_cols) scan_limit = term->term_cols;
    if (scan_limit > ANSI_BUFFER_COLS) scan_limit = ANSI_BUFFER_COLS;

    int len = 0;
    for (int x = 0; x < scan_limit && len < max_cells; x++) {
        AnsiCell cell = src[x];
        if (cell.character == 0) cell.character = ' ';

        // If cursor is visible and at this position, render cursor instead
        if (term->cursor_visible && term->cursor_y == row && term->cursor_x == x) {
            cell.character = '_';  // Render cursor as underscore
        }
        out[len++] = cell;
    }

    // Trim trailing blanks (but not if cursor is at the end). A space carrying a
    // non-default background is kept: that is how coloured status bars and
    // selection highlights fill out to the end of a row.
    bool cursor_at_end = (term->cursor_visible && term->cursor_y == row &&
                          term->cursor_x >= len - 1);
    if (!cursor_at_end) {
        while (len > 0 && out[len - 1].character == ' ' &&
               out[len - 1].bg_color == ANSI_DEFAULT_BG) {
            len--;
        }
    }

    return len;
}

void ansi_get_line(AnsiTerminal* term, int row, char* output, int max_len) {
    if (!term || !output || max_len < 1) {
        if (output && max_len > 0) output[0] = '\0';
        return;
    }

    AnsiCell cells[ANSI_BUFFER_COLS];
    int max_cells = max_len - 1;
    if (max_cells > ANSI_BUFFER_COLS) max_cells = ANSI_BUFFER_COLS;

    int len = ansi_get_line_cells(term, row, cells, max_cells);
    for (int i = 0; i < len; i++) {
        output[i] = cells[i].character;
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
