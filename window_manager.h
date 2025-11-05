#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "font-render.h"
#include "types.h"

#define MAX_WINDOWS 32
#define MAX_WINDOW_NAME 64
#define MAX_TEXT_LENGTH 16384
#define MAX_FONT_PATH 256

typedef struct {
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    TextureID texture;
    int vertexCount;
} NineSlice;

typedef struct {
    ScreenCoord position;
    WindowSize size;
    BorderSize borderLeft, borderRight, borderTop, borderBottom;
} NineSliceParams;

typedef enum {
    WINDOW_STATIC,      // Just displays text
    WINDOW_INTERACTIVE, // Can receive input
    WINDOW_TIMED,       // Appears for a set duration
    WINDOW_TRIGGERED    // Appears based on events
} WindowType;

typedef enum {
    INPUT_NONE,         // No input
    INPUT_TEXT,         // Text input
    INPUT_CHOICE,       // Multiple choice selection
    INPUT_NUMERIC       // Numeric input only
} InputType;

typedef enum {
    ANCHOR_TOP_LEFT,
    ANCHOR_TOP_CENTER,
    ANCHOR_TOP_RIGHT,
    ANCHOR_CENTER_LEFT,
    ANCHOR_CENTER,
    ANCHOR_CENTER_RIGHT,
    ANCHOR_BOTTOM_LEFT,
    ANCHOR_BOTTOM_CENTER,
    ANCHOR_BOTTOM_RIGHT
} WindowAnchor;

typedef struct {
    WindowName name;
    WindowType type;
    InputType input_type;
    WindowAnchor anchor;
    
    // Position and size
    ScreenCoord position;           // Position (can be relative to anchor)
    WindowSize size;                // Size in pixels
    ScreenPercent size_percent_w, size_percent_h;  // Size as percentage of screen
    bool use_percent_size;          // Flag to use percentage sizing
    
    // Appearance
    FontPath font_path;
    FontPath texture_path;          // Path to 9-slice texture (32x32 PNG)
    TextColor text_color;           // Text color (RGBA)
    FontSize font_size;
    BorderSize border_size;
    LineSpacing line_spacing;       // Line spacing multiplier
    TextMargins text_margins;       // All text margins in one struct
    float center_alpha;             // Transparency for center (0.0 to 1.0, borders stay opaque)
    
    // Content
    char text[MAX_TEXT_LENGTH];
    char input_buffer[MAX_TEXT_LENGTH];
    char wrapped_text[MAX_TEXT_LENGTH * 4];  // Buffer for wrapped text
    int text_scroll_offset;  // Line offset for scrolling
    int max_visible_lines;   // How many lines fit in window
    
    // Auto-typing
    char full_text[MAX_TEXT_LENGTH];  // Complete text to type
    TextPosition typing_position;     // Current position in typing
    float typing_timer;               // Timer for typing speed
    TypingSpeed typing_speed;         // Characters per second
    bool auto_typing;                 // Flag for auto-typing mode
    
    // Behavior
    Duration display_duration;        // For timed windows
    Duration trigger_delay;           // Delay before showing
    EventName trigger_event;          // Event name that triggers window
    
    // State
    bool active;
    bool visible;
    float timer;
    TextPosition cursor_pos;     // For text input
    int selected_choice;         // For choice input

    // Menu items (for menu type windows)
    char menu_header[256];       // Header text for menu
    FontPath menu_header_font_path;  // Font path for menu header
    FontSize menu_header_font_size;  // Font size for menu header
    TextColor menu_header_bg_color;  // Background color for menu header
    int menu_header_bg_padding_x;    // Horizontal padding for header background
    int menu_header_bg_padding_y;    // Vertical padding for header background
    char menu_items[32][256];    // Up to 32 menu items, each up to 256 chars
    int menu_item_count;         // Number of menu items

    // Rendering
    NineSlice nine_slice;
    FontInfo* font;
    FontInfo* menu_header_font;  // Separate font for menu header
} Window;

typedef struct {
    Window windows[MAX_WINDOWS];
    int window_count;
    WindowID active_window;
    
    // Rendering context
    ShaderProgram shader_program;
    ShaderProgram font_shader_program;
    float projection_matrix[16];
    
    // Screen dimensions
    ScreenSize screen_size;
    
    // Input state
    char current_input[MAX_TEXT_LENGTH];
    bool input_active;
} WindowManager;

// Core functions
int wm_init(WindowManager* wm, ScreenSize screen_size);
void wm_cleanup(WindowManager* wm);
int wm_load_config(WindowManager* wm, const char* config_file);
void wm_update(WindowManager* wm, DeltaTime delta_time);
void wm_render(WindowManager* wm);

// Window management
WindowID wm_create_window(WindowManager* wm, WindowName name);
Window* wm_get_window(WindowManager* wm, WindowName name);
Window* wm_get_window_by_id(WindowManager* wm, WindowID id);
void wm_show_window(WindowManager* wm, WindowName name);
void wm_hide_window(WindowManager* wm, WindowName name);
void wm_trigger_event(WindowManager* wm, EventName event_name);

// Input handling
void wm_handle_key(WindowManager* wm, int key, int action);
void wm_handle_char(WindowManager* wm, unsigned int codepoint);
void wm_handle_mouse(WindowManager* wm, double x, double y, int button, int action);

// Utility functions
ScreenCoord wm_calculate_position(WindowManager* wm, Window* window);
void wm_calculate_dimensions(WindowManager* wm, Window* window);
void wm_create_window_geometry(WindowManager* wm, Window* window);

// Text processing functions
float wm_measure_text_width(FontInfo* font, const char* text, FontSize font_size);
void wm_wrap_text(Window* window, const char* text, uint32_t max_width);
void wm_calculate_text_layout(Window* window);
void wm_scroll_text(Window* window, int lines);
void wm_render_scrollable_text(Window* window, ScreenCoord position);
void wm_render_scrollable_text_partial(Window* window, ScreenCoord position, TextPosition max_chars);
void wm_render_text_partial(FontInfo* font, const char* text, ScreenCoord position, FontSize font_size, TextPosition max_chars, LineSpacing line_spacing);

// 9-slice functions
void createNineSlice(NineSlice* nineSlice, NineSliceParams* params, WindowSize texture_size, const char* texture_path);
void drawNineSlice(NineSlice* nineSlice);

#endif // WINDOW_MANAGER_H