#ifndef ANSI_H
#define ANSI_H

#include <stdbool.h>

#define ANSI_BUFFER_ROWS 1000
#define ANSI_BUFFER_COLS 120

// The alternate screen has no scrollback, so it only ever needs to be as tall
// as the visible screen.
#define ANSI_ALT_ROWS 256

// 16-colour ANSI palette: 0-7 normal, 8-15 bright.
// Index 0 doubles as "default background" and is rendered transparent so the
// window chrome shows through; index 7 is the default foreground.
#define ANSI_COLOR_COUNT 16
#define ANSI_DEFAULT_FG 7
#define ANSI_DEFAULT_BG 0

typedef struct {
    char character;
    unsigned char fg_color;  // 0-15 (ANSI colors)
    unsigned char bg_color;  // 0-15 (ANSI colors)
    bool bold;
    bool underline;
} AnsiCell;

typedef struct {
    AnsiCell buffer[ANSI_BUFFER_ROWS][ANSI_BUFFER_COLS];
    int cursor_x;
    int cursor_y;

    // Scrolling region (DECSTBM), as screen row indices. Honoured on the
    // alternate screen; the normal screen always scrolls into scrollback.
    int scroll_top;
    int scroll_bottom;
    unsigned char current_fg;
    unsigned char current_bg;
    bool current_bold;
    bool current_underline;
    bool current_reverse;   // SGR 7: fg and bg are swapped when cells are written
    bool cursor_visible;
    bool insert_mode;
    bool dirty;  // Needs redraw
    bool acs_mode;  // Alternate Character Set mode (for box drawing)

    // Alternate screen (DEC private modes 47 / 1047 / 1049). Full-screen
    // programs draw here, so the scrollback underneath is still intact when
    // they exit instead of being painted over.
    AnsiCell alt_buffer[ANSI_ALT_ROWS][ANSI_BUFFER_COLS];
    int alt_line_len[ANSI_ALT_ROWS];
    bool alt_screen;

    // Cursor saved by DECSC / CSI s, and separately across an alt-screen switch.
    int saved_cursor_x, saved_cursor_y;
    int alt_saved_cursor_x, alt_saved_cursor_y;

    // Advertised screen size - this is what the child process was told via
    // TIOCSWINSZ, and therefore where autowrap must happen. It is NOT the
    // buffer size: the buffer is oversized so a window resize does not have to
    // reallocate. Wrapping at the buffer width instead desyncs the cursor from
    // what the child believes, which makes long lines overwrite themselves.
    int term_cols;
    int term_rows;

    // Deferred wrap (DEC autowrap): writing a glyph into the last column parks
    // the cursor there instead of moving straight to the next row. The wrap
    // happens when the *next* glyph arrives, so a line that exactly fills the
    // width followed by CR/LF does not produce a blank line.
    bool pending_wrap;

    // Ring buffer optimization: instead of shifting entire buffer on scroll,
    // we just increment first_row and treat buffer as circular
    int first_row;  // Physical row index that maps to logical row 0
    int line_len[ANSI_BUFFER_ROWS];  // Track actual used length per row for faster rendering

    // Parser state for handling split escape sequences across buffer boundaries
    char escape_buffer[256];  // Buffer to accumulate partial escape sequences
    int escape_buffer_len;    // Current length of data in escape_buffer
    bool in_escape;           // True if we're currently accumulating an escape sequence

    // A multi-byte UTF-8 character can straddle two PTY reads; its leading
    // bytes wait here for the rest to arrive.
    char utf8_pending[4];
    int utf8_pending_len;
} AnsiTerminal;

// Initialize ANSI terminal
void ansi_init(AnsiTerminal* term);

// Set the advertised screen size (autowrap column and row count). Pass the
// same values handed to the child process via TIOCSWINSZ.
void ansi_set_size(AnsiTerminal* term, int cols, int rows);

// Process ANSI escape sequences and raw text
void ansi_process_output(AnsiTerminal* term, const char* data, int length);

// Get rendered line for display (characters only, NUL-terminated)
void ansi_get_line(AnsiTerminal* term, int row, char* output, int max_len);

// Get a rendered line as attributed cells, preserving colour/bold/underline.
// Applies the same cursor overlay and trailing-cell trim as ansi_get_line().
// Returns the number of cells written (0 to max_cells).
int ansi_get_line_cells(AnsiTerminal* term, int row, AnsiCell* out, int max_cells);

// Clear the terminal
void ansi_clear(AnsiTerminal* term);

// Get cursor position
void ansi_get_cursor(AnsiTerminal* term, int* x, int* y);

// True while a full-screen program has the alternate screen active. Callers
// use this to render from screen row 0 and to disable scrollback scrolling.
bool ansi_is_alt_screen(AnsiTerminal* term);

// Number of rows the active screen addresses.
int ansi_screen_rows(AnsiTerminal* term);

// Check if terminal needs redraw
bool ansi_is_dirty(AnsiTerminal* term);
void ansi_mark_clean(AnsiTerminal* term);

#endif // ANSI_H
