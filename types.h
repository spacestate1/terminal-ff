#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// Strong type system to prevent mixing up different kinds of values

// =============================================================================
// Window Management Types
// =============================================================================

// Window ID - prevents mixing up window indices with other integers
typedef struct {
    int32_t value;
} WindowID;

#define INVALID_WINDOW_ID ((WindowID){-1})
#define WINDOW_ID(val) ((WindowID){val})

static inline bool window_id_equals(WindowID a, WindowID b) {
    return a.value == b.value;
}

static inline bool window_id_is_valid(WindowID id) {
    return id.value >= 0;
}

// =============================================================================
// Coordinate System Types
// =============================================================================

// Screen coordinates - prevents mixing up pixel positions with sizes
typedef struct {
    int32_t x;
    int32_t y;
} ScreenCoord;

#define SCREEN_COORD(x_val, y_val) ((ScreenCoord){x_val, y_val})

// Screen dimensions - prevents mixing up width/height with coordinates
typedef struct {
    uint32_t width;
    uint32_t height;
} ScreenSize;

#define SCREEN_SIZE(w, h) ((ScreenSize){w, h})

// Window dimensions - prevents mixing up window sizes with screen sizes
typedef struct {
    uint32_t width;
    uint32_t height;
} WindowSize;

#define WINDOW_SIZE(w, h) ((WindowSize){w, h})

// Text margins - prevents mixing up different margin types
typedef struct {
    uint32_t top;
    uint32_t bottom;
    uint32_t left;
    uint32_t right;
} TextMargins;

#define TEXT_MARGINS(t, b, l, r) ((TextMargins){t, b, l, r})

// Border size - prevents mixing up border with other measurements
typedef struct {
    uint32_t size;
} BorderSize;

#define BORDER_SIZE(val) ((BorderSize){val})

// =============================================================================
// Font and Text Types
// =============================================================================

// Font size - prevents mixing up font size with other float values
typedef struct {
    float value;
} FontSize;

#define FONT_SIZE(val) ((FontSize){val})

// Line spacing - prevents mixing up line spacing with font size
typedef struct {
    float value;
} LineSpacing;

#define LINE_SPACING(val) ((LineSpacing){val})

// Text position in buffer - prevents mixing up with pixel coordinates
typedef struct {
    uint32_t position;
} TextPosition;

#define TEXT_POSITION(val) ((TextPosition){val})

// Typing speed - prevents mixing up with other timing values
typedef struct {
    float chars_per_second;
} TypingSpeed;

#define TYPING_SPEED(val) ((TypingSpeed){val})

// =============================================================================
// Timing Types
// =============================================================================

// Duration in seconds - prevents mixing up different time measurements
typedef struct {
    float seconds;
} Duration;

#define DURATION_SECONDS(val) ((Duration){val})
#define DURATION_MS(val) ((Duration){(val) / 1000.0f})

// Delta time - prevents mixing up frame time with absolute time
typedef struct {
    float seconds;
} DeltaTime;

#define DELTA_TIME(val) ((DeltaTime){val})

// =============================================================================
// Percentage Types
// =============================================================================

// Screen percentage - prevents mixing up percentages with pixel values
typedef struct {
    float value;  // 0.0 to 1.0
} ScreenPercent;

#define SCREEN_PERCENT(val) ((ScreenPercent){val})

static inline ScreenPercent screen_percent_from_int(int percent) {
    return SCREEN_PERCENT((float)percent / 100.0f);
}

// =============================================================================
// String Types for Different Purposes
// =============================================================================

// Window name - prevents mixing up window names with other strings
typedef struct {
    char value[64];
} WindowName;

static inline WindowName window_name_from_string(const char* str) {
    WindowName name = {0};
    if (str) {
        strncpy(name.value, str, sizeof(name.value) - 1);
    }
    return name;
}

static inline bool window_name_equals(WindowName a, WindowName b) {
    return strcmp(a.value, b.value) == 0;
}

static inline bool window_name_equals_string(WindowName name, const char* str) {
    return str && strcmp(name.value, str) == 0;
}

// Font path - prevents mixing up font paths with other file paths
typedef struct {
    char value[256];
} FontPath;

static inline FontPath font_path_from_string(const char* str) {
    FontPath path = {0};
    if (str) {
        strncpy(path.value, str, sizeof(path.value) - 1);
    }
    return path;
}

// Event name - prevents mixing up event names with window names
typedef struct {
    char value[64];
} EventName;

static inline EventName event_name_from_string(const char* str) {
    EventName name = {0};
    if (str) {
        strncpy(name.value, str, sizeof(name.value) - 1);
    }
    return name;
}

static inline bool event_name_equals(EventName a, EventName b) {
    return strcmp(a.value, b.value) == 0;
}

static inline bool event_name_equals_string(EventName name, const char* str) {
    return str && strcmp(name.value, str) == 0;
}

// Text color - RGBA values (0.0 to 1.0)
typedef struct {
    float r, g, b, a;
} TextColor;

#define TEXT_COLOR(r, g, b, a) ((TextColor){(r), (g), (b), (a)})
#define TEXT_COLOR_BLACK TEXT_COLOR(0.0f, 0.0f, 0.0f, 1.0f)
#define TEXT_COLOR_WHITE TEXT_COLOR(1.0f, 1.0f, 1.0f, 1.0f)
#define TEXT_COLOR_RED TEXT_COLOR(1.0f, 0.0f, 0.0f, 1.0f)
#define TEXT_COLOR_GREEN TEXT_COLOR(0.0f, 1.0f, 0.0f, 1.0f)
#define TEXT_COLOR_BLUE TEXT_COLOR(0.0f, 0.0f, 1.0f, 1.0f)

static inline TextColor text_color_from_string(const char* str) {
    TextColor color = TEXT_COLOR_BLACK;  // Default to black
    if (str) {
        // Parse hex color (#RRGGBB or #RRGGBBAA)
        if (str[0] == '#') {
            unsigned int r, g, b, a = 255;
            if (strlen(str) >= 7) {
                sscanf(str + 1, "%02x%02x%02x", &r, &g, &b);
                if (strlen(str) >= 9) {
                    sscanf(str + 7, "%02x", &a);
                }
                color.r = r / 255.0f;
                color.g = g / 255.0f;
                color.b = b / 255.0f;
                color.a = a / 255.0f;
            }
        }
        // Parse named colors
        else if (strcmp(str, "black") == 0) color = TEXT_COLOR_BLACK;
        else if (strcmp(str, "white") == 0) color = TEXT_COLOR_WHITE;
        else if (strcmp(str, "red") == 0) color = TEXT_COLOR_RED;
        else if (strcmp(str, "green") == 0) color = TEXT_COLOR_GREEN;
        else if (strcmp(str, "blue") == 0) color = TEXT_COLOR_BLUE;
        // Parse RGB values (e.g., "255,128,0" or "1.0,0.5,0.0")
        else {
            float r, g, b, a = 1.0f;
            if (sscanf(str, "%f,%f,%f,%f", &r, &g, &b, &a) >= 3) {
                // If values are > 1, assume they're in 0-255 range
                if (r > 1.0f || g > 1.0f || b > 1.0f) {
                    color.r = r / 255.0f;
                    color.g = g / 255.0f;
                    color.b = b / 255.0f;
                    color.a = a / 255.0f;
                } else {
                    color.r = r;
                    color.g = g;
                    color.b = b;
                    color.a = a;
                }
            }
        }
    }
    return color;
}

// =============================================================================
// OpenGL Types
// =============================================================================

// Texture ID - prevents mixing up texture IDs with other GLuint values
typedef struct {
    uint32_t id;
} TextureID;

#define TEXTURE_ID(val) ((TextureID){val})
#define INVALID_TEXTURE_ID ((TextureID){0})

// Shader program ID - prevents mixing up shaders with textures
typedef struct {
    uint32_t id;
} ShaderProgram;

#define SHADER_PROGRAM(val) ((ShaderProgram){val})
#define INVALID_SHADER_PROGRAM ((ShaderProgram){0})

// =============================================================================
// Utility Functions
// =============================================================================

// Convert screen percentage to pixel coordinates
static inline ScreenCoord screen_percent_to_coord(ScreenPercent percent_x, ScreenPercent percent_y, ScreenSize screen_size) {
    return SCREEN_COORD(
        (int32_t)(percent_x.value * screen_size.width),
        (int32_t)(percent_y.value * screen_size.height)
    );
}

// Convert screen percentage to pixel size
static inline WindowSize screen_percent_to_size(ScreenPercent percent_w, ScreenPercent percent_h, ScreenSize screen_size) {
    return WINDOW_SIZE(
        (uint32_t)(percent_w.value * screen_size.width),
        (uint32_t)(percent_h.value * screen_size.height)
    );
}

#endif // TYPES_H