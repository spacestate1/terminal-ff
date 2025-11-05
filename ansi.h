#ifndef ANSI_H
#define ANSI_H

#include <stdbool.h>

#define ANSI_BUFFER_ROWS 100
#define ANSI_BUFFER_COLS 120

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
    int scroll_top;
    int scroll_bottom;
    unsigned char current_fg;
    unsigned char current_bg;
    bool current_bold;
    bool current_underline;
    bool cursor_visible;
    bool insert_mode;
    bool dirty;  // Needs redraw
    bool acs_mode;  // Alternate Character Set mode (for box drawing)
} AnsiTerminal;

// Initialize ANSI terminal
void ansi_init(AnsiTerminal* term);

// Process ANSI escape sequences and raw text
void ansi_process_output(AnsiTerminal* term, const char* data, int length);

// Get rendered line for display
void ansi_get_line(AnsiTerminal* term, int row, char* output, int max_len);

// Clear the terminal
void ansi_clear(AnsiTerminal* term);

// Get cursor position
void ansi_get_cursor(AnsiTerminal* term, int* x, int* y);

// Check if terminal needs redraw
bool ansi_is_dirty(AnsiTerminal* term);
void ansi_mark_clean(AnsiTerminal* term);

#endif // ANSI_H
