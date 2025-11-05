// Working window manager implementation with type system
#include "window_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// Forward declarations
extern GLuint loadTexture(const char* path);
void wrap_text_to_window(Window* window);
float calculate_word_width(Window* window, const char* word_start, size_t word_length);
int count_lines_in_text(const char* text);
int calculate_max_visible_lines(Window* window);
void scroll_text_up_one_line(char* text);

// 9-slice rendering implementation
void createNineSlice(NineSlice* nineSlice, NineSliceParams* params, WindowSize texture_size, const char* texture_path) {
    // Load the 9-slice texture
    nineSlice->texture = TEXTURE_ID(loadTexture(texture_path));
    nineSlice->vao = 0;
    nineSlice->vbo = 0; 
    nineSlice->ebo = 0;
    nineSlice->vertexCount = 54; // 9 quads * 6 vertices per quad
    
    printf("✓ Created 9-slice with texture ID: %u\n", nineSlice->texture.id);
    (void)params; (void)texture_size;
}

void drawNineSlice(NineSlice* nineSlice) {
    if (nineSlice->texture.id == 0) return;
    
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, nineSlice->texture.id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // 9-slice rendering would go here
    // For now, we'll render in the main render function
    
    glDisable(GL_TEXTURE_2D);
}

// Measure text width for centering
float wm_measure_text_width(FontInfo* font, const char* text, FontSize font_size) {
    if (!font || !text) return 0.0f;

    float total_width = 0.0f;
    while (*text) {
        if (*text == '\n') {
            // Newlines don't add to width
            text++;
            continue;
        }

        int char_index = *text - font->first_char;
        if (char_index >= 0 && char_index < font->last_char - font->first_char) {
            // Get advance_x from metrics (index 6 in the 7-value metric array)
            total_width += font_size.value * font->metrics[char_index * 7 + 6];
        } else {
            // Default advance for unknown characters
            total_width += font_size.value * 5.0f;
        }
        text++;
    }

    return total_width;
}

void render_nine_slice_border(GLuint texture_id, float x, float y, float width, float height, float border_size, float center_alpha) {
    if (texture_id == 0) return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // 9-slice texture coordinates (assuming 32x32 texture with 8px borders)
    float tex_size = 32.0f;
    float border_uv = 8.0f / tex_size;
    float center_uv = 1.0f - 2.0f * border_uv;
    
    // Calculate positions
    float x1 = x, x2 = x + border_size, x3 = x + width - border_size, x4 = x + width;
    float y1 = y, y2 = y + border_size, y3 = y + height - border_size, y4 = y + height;
    
    glBegin(GL_QUADS);

    // Borders remain opaque
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Top-left corner
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x1, y1);
    glTexCoord2f(border_uv, 0.0f); glVertex2f(x2, y1);
    glTexCoord2f(border_uv, border_uv); glVertex2f(x2, y2);
    glTexCoord2f(0.0f, border_uv); glVertex2f(x1, y2);

    // Top edge
    glTexCoord2f(border_uv, 0.0f); glVertex2f(x2, y1);
    glTexCoord2f(border_uv + center_uv, 0.0f); glVertex2f(x3, y1);
    glTexCoord2f(border_uv + center_uv, border_uv); glVertex2f(x3, y2);
    glTexCoord2f(border_uv, border_uv); glVertex2f(x2, y2);

    // Top-right corner
    glTexCoord2f(border_uv + center_uv, 0.0f); glVertex2f(x3, y1);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x4, y1);
    glTexCoord2f(1.0f, border_uv); glVertex2f(x4, y2);
    glTexCoord2f(border_uv + center_uv, border_uv); glVertex2f(x3, y2);

    // Left edge
    glTexCoord2f(0.0f, border_uv); glVertex2f(x1, y2);
    glTexCoord2f(border_uv, border_uv); glVertex2f(x2, y2);
    glTexCoord2f(border_uv, border_uv + center_uv); glVertex2f(x2, y3);
    glTexCoord2f(0.0f, border_uv + center_uv); glVertex2f(x1, y3);

    glEnd();

    // Center uses semi-transparent color
    glColor4f(1.0f, 1.0f, 1.0f, center_alpha);
    glBegin(GL_QUADS);

    // Center
    glTexCoord2f(border_uv, border_uv); glVertex2f(x2, y2);
    glTexCoord2f(border_uv + center_uv, border_uv); glVertex2f(x3, y2);
    glTexCoord2f(border_uv + center_uv, border_uv + center_uv); glVertex2f(x3, y3);
    glTexCoord2f(border_uv, border_uv + center_uv); glVertex2f(x2, y3);

    glEnd();

    // Continue with opaque borders
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);

    // Right edge
    glTexCoord2f(border_uv + center_uv, border_uv); glVertex2f(x3, y2);
    glTexCoord2f(1.0f, border_uv); glVertex2f(x4, y2);
    glTexCoord2f(1.0f, border_uv + center_uv); glVertex2f(x4, y3);
    glTexCoord2f(border_uv + center_uv, border_uv + center_uv); glVertex2f(x3, y3);

    // Bottom-left corner
    glTexCoord2f(0.0f, border_uv + center_uv); glVertex2f(x1, y3);
    glTexCoord2f(border_uv, border_uv + center_uv); glVertex2f(x2, y3);
    glTexCoord2f(border_uv, 1.0f); glVertex2f(x2, y4);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x1, y4);

    // Bottom edge
    glTexCoord2f(border_uv, border_uv + center_uv); glVertex2f(x2, y3);
    glTexCoord2f(border_uv + center_uv, border_uv + center_uv); glVertex2f(x3, y3);
    glTexCoord2f(border_uv + center_uv, 1.0f); glVertex2f(x3, y4);
    glTexCoord2f(border_uv, 1.0f); glVertex2f(x2, y4);

    // Bottom-right corner
    glTexCoord2f(border_uv + center_uv, border_uv + center_uv); glVertex2f(x3, y3);
    glTexCoord2f(1.0f, border_uv + center_uv); glVertex2f(x4, y3);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x4, y4);
    glTexCoord2f(border_uv + center_uv, 1.0f); glVertex2f(x3, y4);

    glEnd();
    glDisable(GL_TEXTURE_2D);
}

int wm_init(WindowManager* wm, ScreenSize screen_size) {
    if (!wm) return 0;
    
    memset(wm, 0, sizeof(WindowManager));
    wm->screen_size = screen_size;
    wm->window_count = 0;
    wm->input_active = false;
    wm->active_window = INVALID_WINDOW_ID;
    
    printf("✓ Window manager initialized with screen size: %dx%d\n", 
           screen_size.width, screen_size.height);
    
    // Verify 1920x1080 resolution
    if (screen_size.width == 1920 && screen_size.height == 1080) {
        printf("✓ Confirmed 1920x1080 resolution\n");
    } else {
        printf("⚠ Warning: Expected 1920x1080, got %dx%d\n", 
               screen_size.width, screen_size.height);
    }
    
    return 1;
}

WindowID wm_create_window(WindowManager* wm, WindowName name) {
    if (!wm || wm->window_count >= MAX_WINDOWS) {
        return INVALID_WINDOW_ID;
    }
    
    Window* window = &wm->windows[wm->window_count];
    memset(window, 0, sizeof(Window));
    
    // Initialize with type-safe defaults
    window->name = name;
    window->type = WINDOW_STATIC;
    window->input_type = INPUT_NONE;
    window->anchor = ANCHOR_CENTER;
    window->position = SCREEN_COORD(0, 0);
    window->size = WINDOW_SIZE(400, 300);
    window->size_percent_w = SCREEN_PERCENT(0.0f);
    window->size_percent_h = SCREEN_PERCENT(0.0f);
    window->use_percent_size = false;
    window->texture_path = font_path_from_string("assets/gui/9-slice-basice8.png"); // Default texture
    window->text_color = TEXT_COLOR_BLACK;  // Default to black text
    window->font_size = FONT_SIZE(16.0f);
    window->border_size = BORDER_SIZE(8);
    window->line_spacing = LINE_SPACING(1.0f);
    window->text_margins = TEXT_MARGINS(10, 10, 10, 10);
    window->center_alpha = 1.0f;  // Default to fully opaque
    window->typing_position = TEXT_POSITION(0);
    window->typing_speed = TYPING_SPEED(15.0f);
    window->auto_typing = false;
    window->typing_timer = 0.0f;
    window->display_duration = DURATION_SECONDS(0.0f);
    window->trigger_delay = DURATION_SECONDS(0.0f);
    window->cursor_pos = TEXT_POSITION(0);
    window->visible = false;
    window->active = false;
    window->menu_header[0] = '\0';  // Initialize menu header as empty
    window->menu_header_font_path = font_path_from_string("");  // Initialize header font path as empty
    window->menu_header_font_size = FONT_SIZE(1.5f);  // Default to 1.5x size for headers
    window->menu_header_bg_color = TEXT_COLOR(0.0f, 0.0f, 0.0f, 0.0f);  // Default to transparent
    window->menu_header_bg_padding_x = 0;  // Default no horizontal padding
    window->menu_header_bg_padding_y = 0;  // Default no vertical padding
    window->menu_item_count = 0;  // Initialize menu items
    window->menu_header_font = NULL;  // Initialize header font pointer

    // Don't load texture here - wait until after config is loaded
    // The texture will be loaded in wm_show_window()
    memset(&window->nine_slice, 0, sizeof(NineSlice));
    
    return WINDOW_ID(wm->window_count++);
}

Window* wm_get_window(WindowManager* wm, WindowName name) {
    if (!wm) return NULL;
    
    for (int i = 0; i < wm->window_count; i++) {
        if (window_name_equals(wm->windows[i].name, name)) {
            return &wm->windows[i];
        }
    }
    return NULL;
}

ScreenCoord wm_calculate_position(WindowManager* wm, Window* window) {
    if (!wm || !window) return SCREEN_COORD(0, 0);
    
    int screen_w = wm->screen_size.width;
    int screen_h = wm->screen_size.height;
    
    int window_w = window->size.width;
    int window_h = window->size.height;
    
    // Convert percentage sizes if needed (support mixed percentage/pixel sizing)
    if (window->use_percent_size) {
        // Use percentage for width if it was set
        if (window->size_percent_w.value > 0.0f) {
            window_w = (int)(screen_w * window->size_percent_w.value / 100.0f);
        }
        // Use percentage for height if it was set  
        if (window->size_percent_h.value > 0.0f) {
            window_h = (int)(screen_h * window->size_percent_h.value / 100.0f);
        }
        window->size = WINDOW_SIZE(window_w, window_h);
    }
    
    int base_x = 0, base_y = 0;
    
    // Calculate anchor position
    switch (window->anchor) {
        case ANCHOR_TOP_LEFT:
            base_x = 0; base_y = 0;
            break;
        case ANCHOR_TOP_CENTER:
            base_x = screen_w / 2; base_y = 0;
            break;
        case ANCHOR_TOP_RIGHT:
            base_x = screen_w; base_y = 0;
            break;
        case ANCHOR_CENTER_LEFT:
            base_x = 0; base_y = screen_h / 2;
            break;
        case ANCHOR_CENTER:
            base_x = screen_w / 2; base_y = screen_h / 2;
            break;
        case ANCHOR_CENTER_RIGHT:
            base_x = screen_w; base_y = screen_h / 2;
            break;
        case ANCHOR_BOTTOM_LEFT:
            base_x = 0; base_y = screen_h;
            break;
        case ANCHOR_BOTTOM_CENTER:
            base_x = screen_w / 2; base_y = screen_h;
            break;
        case ANCHOR_BOTTOM_RIGHT:
            base_x = screen_w; base_y = screen_h;
            break;
    }
    
    // Apply window offset and adjust for window size
    int final_x = base_x + window->position.x;
    int final_y = base_y + window->position.y;
    
    // Adjust for anchor position relative to window
    if (window->anchor == ANCHOR_TOP_CENTER || window->anchor == ANCHOR_CENTER || window->anchor == ANCHOR_BOTTOM_CENTER) {
        final_x -= window_w / 2;
    } else if (window->anchor == ANCHOR_TOP_RIGHT || window->anchor == ANCHOR_CENTER_RIGHT || window->anchor == ANCHOR_BOTTOM_RIGHT) {
        final_x -= window_w;
    }
    
    if (window->anchor == ANCHOR_CENTER_LEFT || window->anchor == ANCHOR_CENTER || window->anchor == ANCHOR_CENTER_RIGHT) {
        final_y -= window_h / 2;
    } else if (window->anchor == ANCHOR_BOTTOM_LEFT || window->anchor == ANCHOR_BOTTOM_CENTER || window->anchor == ANCHOR_BOTTOM_RIGHT) {
        final_y -= window_h;
    }
    
    return SCREEN_COORD(final_x, final_y);
}

int wm_load_config(WindowManager* wm, const char* filename) {
    if (!wm || !filename) return 0;
    
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Failed to open config file: %s\n", filename);
        return 0;
    }
    
    char line[MAX_TEXT_LENGTH];
    Window* current_window = NULL;
    
    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;
        
        // Parse window section headers
        if (line[0] == '[' && strstr(line, "window:")) {
            char window_name[MAX_WINDOW_NAME];
            if (sscanf(line, "[window:%63[^]]", window_name) == 1) {
                WindowID window_id = wm_create_window(wm, window_name_from_string(window_name));
                if (window_id_is_valid(window_id)) {
                    current_window = &wm->windows[window_id.value];
                    printf("Created window: %s\n", window_name);
                }
            }
            continue;
        }
        
        // Parse key-value pairs
        if (current_window && strchr(line, '=')) {
            char key[64], value[MAX_TEXT_LENGTH];
            char* equals_pos = strchr(line, '=');
            if (equals_pos != NULL) {
                // Extract key
                size_t key_len = equals_pos - line;
                if (key_len < sizeof(key)) {
                    strncpy(key, line, key_len);
                    key[key_len] = '\0';
                    
                    // Extract value (everything after =)
                    strncpy(value, equals_pos + 1, sizeof(value) - 1);
                    value[sizeof(value) - 1] = '\0';
                    
                    // Remove any whitespace from key
                    char* k = key; while (*k == ' ') k++;
                    // Remove leading whitespace from value
                    char* v = value; while (*v == ' ') v++;
                
                if (strcmp(k, "type") == 0) {
                    current_window->type = strcmp(v, "timed") == 0 ? WINDOW_TIMED : WINDOW_STATIC;
                } else if (strcmp(k, "anchor") == 0) {
                    if (strcmp(v, "center") == 0) current_window->anchor = ANCHOR_CENTER;
                    else if (strcmp(v, "bottom_center") == 0) current_window->anchor = ANCHOR_BOTTOM_CENTER;
                    else if (strcmp(v, "top_center") == 0) current_window->anchor = ANCHOR_TOP_CENTER;
                } else if (strcmp(k, "x") == 0) {
                    current_window->position.x = atoi(v);
                } else if (strcmp(k, "y") == 0) {
                    current_window->position.y = atoi(v);
                } else if (strcmp(k, "width") == 0) {
                    if (strchr(v, '%')) {
                        current_window->size_percent_w = SCREEN_PERCENT(atof(v));
                        current_window->use_percent_size = true;
                    } else {
                        current_window->size.width = atoi(v);
                    }
                } else if (strcmp(k, "height") == 0) {
                    if (strchr(v, '%')) {
                        current_window->size_percent_h = SCREEN_PERCENT(atof(v));
                        current_window->use_percent_size = true;
                    } else {
                        current_window->size.height = atoi(v);
                    }
                } else if (strcmp(k, "font_size") == 0) {
                    current_window->font_size = FONT_SIZE(atof(v));
                } else if (strcmp(k, "line_spacing") == 0) {
                    current_window->line_spacing = LINE_SPACING(atof(v));
                } else if (strcmp(k, "border_size") == 0) {
                    current_window->border_size = BORDER_SIZE(atoi(v));
                } else if (strcmp(k, "center_alpha") == 0 || strcmp(k, "transparency") == 0) {
                    current_window->center_alpha = atof(v);
                } else if (strcmp(k, "text_margin_top") == 0) {
                    current_window->text_margins.top = atoi(v);
                } else if (strcmp(k, "text_margin_bottom") == 0) {
                    current_window->text_margins.bottom = atoi(v);
                } else if (strcmp(k, "text_margin_left") == 0) {
                    current_window->text_margins.left = atoi(v);
                } else if (strcmp(k, "text_margin_right") == 0) {
                    current_window->text_margins.right = atoi(v);
                } else if (strcmp(k, "auto_typing") == 0) {
                    current_window->auto_typing = strcmp(v, "true") == 0;
                } else if (strcmp(k, "typing_speed") == 0) {
                    current_window->typing_speed = TYPING_SPEED(atof(v));
                } else if (strcmp(k, "text") == 0) {
                    strncpy(current_window->full_text, v, sizeof(current_window->full_text) - 1);
                } else if (strcmp(k, "font") == 0) {
                    current_window->font_path = font_path_from_string(v);
                    // Load font - convert .png to .json for metrics
                    char json_path[256];
                    strncpy(json_path, v, sizeof(json_path) - 1);
                    json_path[sizeof(json_path) - 1] = '\0';
                    
                    // Replace .png with .json
                    char* ext = strstr(json_path, ".png");
                    if (ext) {
                        strcpy(ext, ".json");
                    }
                    
                    current_window->font = LoadFont(json_path, v);
                    if (!current_window->font) {
                        printf("Warning: Failed to load font %s (metrics: %s)\n", v, json_path);
                        // Try fallback font
                        current_window->font = LoadFont("assets/fonts/bofhfonts.json", "assets/fonts/bofhfonts.png");
                        if (!current_window->font) {
                            fprintf(stderr, "CRITICAL ERROR: Even fallback font failed to load! Cannot continue.\n");
                            fprintf(stderr, "  Primary font: %s\n", v);
                            fprintf(stderr, "  Fallback font: assets/fonts/bofhfonts.png\n");
                            fclose(file);
                            return 0;  // Fail config loading
                        } else {
                            printf("✓ Using fallback font: bofhfonts.png\n");
                        }
                    } else {
                        printf("✓ Loaded font: %s\n", v);
                    }
                } else if (strcmp(k, "texture") == 0) {
                    current_window->texture_path = font_path_from_string(v);
                    printf("  Window texture: %s\n", v);
                } else if (strcmp(k, "text_color") == 0) {
                    current_window->text_color = text_color_from_string(v);
                    printf("  Text color: %s (%.2f, %.2f, %.2f, %.2f)\n", v,
                           current_window->text_color.r, current_window->text_color.g,
                           current_window->text_color.b, current_window->text_color.a);
                } else if (strcmp(k, "menu_header") == 0) {
                    strncpy(current_window->menu_header, v, 255);
                    current_window->menu_header[255] = '\0';
                    printf("  Menu header: %s\n", v);
                } else if (strcmp(k, "menu_header_font") == 0) {
                    current_window->menu_header_font_path = font_path_from_string(v);
                    // Load the header font
                    char png_path[512];
                    strncpy(png_path, v, sizeof(png_path) - 1);
                    png_path[sizeof(png_path) - 1] = '\0';

                    // Replace .json with .png
                    char *ext = strstr(png_path, ".json");
                    if (ext) {
                        strcpy(ext, ".png");
                    }

                    current_window->menu_header_font = LoadFont(v, png_path);
                    if (current_window->menu_header_font) {
                        printf("✓ Loaded header font: %s\n", v);
                    } else {
                        printf("✗ Failed to load header font: %s\n", v);
                    }
                } else if (strcmp(k, "menu_header_font_size") == 0) {
                    current_window->menu_header_font_size = FONT_SIZE(atof(v));
                    printf("  Menu header font size: %.2f\n", atof(v));
                } else if (strcmp(k, "menu_header_bg_color") == 0) {
                    current_window->menu_header_bg_color = text_color_from_string(v);
                    printf("  Menu header BG color: %s (%.2f, %.2f, %.2f, %.2f)\n", v,
                           current_window->menu_header_bg_color.r, current_window->menu_header_bg_color.g,
                           current_window->menu_header_bg_color.b, current_window->menu_header_bg_color.a);
                } else if (strcmp(k, "menu_header_bg_padding_x") == 0) {
                    current_window->menu_header_bg_padding_x = atoi(v);
                    printf("  Menu header BG horizontal padding: %d\n", atoi(v));
                } else if (strcmp(k, "menu_header_bg_padding_y") == 0) {
                    current_window->menu_header_bg_padding_y = atoi(v);
                    printf("  Menu header BG vertical padding: %d\n", atoi(v));
                }
                }
            }
        }
    }
    
    fclose(file);
    printf("✓ Loaded configuration with %d windows\n", wm->window_count);
    
    // Debug output to verify test windows were loaded
    for (int i = 0; i < wm->window_count; i++) {
        if (strstr(wm->windows[i].name.value, "test")) {
            printf("  Found test window: %s\n", wm->windows[i].name.value);
        }
    }
    
    // Print loaded configuration for verification
    for (int i = 0; i < wm->window_count; i++) {
        Window* w = &wm->windows[i];
        printf("  Window %d: %s\n", i, w->name.value);
        printf("    Size: %dx%d (%.1f%% x %.1f%%)\n", 
               w->size.width, w->size.height, 
               w->size_percent_w.value, w->size_percent_h.value);
        printf("    Position: (%d, %d)\n", w->position.x, w->position.y);
        printf("    Font size: %.1f, Line spacing: %.2f\n", 
               w->font_size.value, w->line_spacing.value);
        printf("    Typing speed: %.1f chars/sec, Auto-typing: %s\n", 
               w->typing_speed.chars_per_second, w->auto_typing ? "yes" : "no");
        printf("    Margins: T:%d B:%d L:%d R:%d\n", 
               w->text_margins.top, w->text_margins.bottom, 
               w->text_margins.left, w->text_margins.right);
        printf("    Border size: %d\n", w->border_size.size);
    }
    
    return 1;
}

void wm_show_window(WindowManager* wm, WindowName name) {
    Window* window = wm_get_window(wm, name);
    if (window) {
        window->visible = true;
        window->active = true;
        window->typing_position = TEXT_POSITION(0);
        memset(window->wrapped_text, 0, sizeof(window->wrapped_text));
        
        // Reload the 9-slice texture if needed
        if (window->nine_slice.texture.id == 0) {
            NineSliceParams params = {
                .position = SCREEN_COORD(0, 0),
                .size = window->size,
                .borderLeft = window->border_size,
                .borderRight = window->border_size,
                .borderTop = window->border_size,
                .borderBottom = window->border_size
            };
            createNineSlice(&window->nine_slice, &params, WINDOW_SIZE(32, 32), window->texture_path.value);
        }
        
        // Initial text wrapping setup
        if (window->font) {
            wrap_text_to_window(window);
        }
        
        printf("✓ Showing window: %s (9-slice texture: %s from %s)\n", 
               window->name.value,
               window->nine_slice.texture.id > 0 ? "loaded" : "failed",
               window->texture_path.value);
    }
}

void wm_hide_window(WindowManager* wm, WindowName name) {
    Window* window = wm_get_window(wm, name);
    if (window) {
        window->visible = false;
        window->active = false;
    }
}

// Calculate the pixel width of a word using font metrics
float calculate_word_width(Window* window, const char* word_start, size_t word_length) {
    float word_width = 0.0f;
    
    for (size_t i = 0; i < word_length; i++) {
        char c = word_start[i];
        if (window->font && c >= window->font->first_char && c <= window->font->last_char) {
            int char_index = c - window->font->first_char;
            word_width += window->font_size.value * window->font->metrics[char_index * 7 + 6];
        } else {
            word_width += window->font_size.value * 5; // Default width for unknown chars
        }
    }
    
    return word_width;
}

// Text wrapping function that respects window boundaries and keeps words whole
void wrap_text_to_window(Window* window) {
    if (!window->font) return;
    
    // Calculate available text width (window width minus margins)
    float available_width = window->size.width - window->text_margins.left - window->text_margins.right;
    
    const char* source = window->full_text;
    char* dest = window->wrapped_text;
    size_t max_chars = window->typing_position.position;
    
    float current_line_width = 0.0f;
    size_t dest_pos = 0;
    size_t source_pos = 0;
    
    // Clear the wrapped text buffer
    memset(window->wrapped_text, 0, sizeof(window->wrapped_text));
    
    while (source_pos < max_chars && source[source_pos] != '\0' && dest_pos < sizeof(window->wrapped_text) - 1) {
        // Handle existing newlines
        if (source[source_pos] == '\n') {
            dest[dest_pos++] = '\n';
            current_line_width = 0.0f;
            source_pos++;
            continue;
        }
        
        // Skip whitespace at start of line
        if (current_line_width == 0.0f && source[source_pos] == ' ') {
            source_pos++;
            continue;
        }
        
        // Find the end of the current word (including any trailing space)
        size_t word_start = source_pos;
        size_t word_end = source_pos;

        // Find end of word in FULL text (not limited by typing position)
        // This allows us to predict if the complete word will fit
        while (source[word_end] != '\0' &&
               source[word_end] != ' ' && source[word_end] != '\n') {
            word_end++;
        }

        // Include the trailing space if there is one
        bool has_trailing_space = false;
        if (source[word_end] == ' ') {
            has_trailing_space = true;
        }

        // Calculate actual visible length (can't show more than typed so far)
        size_t visible_end = word_end;
        if (visible_end > max_chars) {
            visible_end = max_chars;
        }
        size_t visible_length = visible_end - word_start;

        // Calculate the width of the COMPLETE word for wrapping decision
        size_t complete_word_length = word_end - word_start;
        float word_width = calculate_word_width(window, &source[word_start], complete_word_length);
        
        // Add space width if there's a trailing space and we're not at the start of a line
        float space_width = 0.0f;
        if (has_trailing_space && current_line_width > 0.0f) {
            if (window->font && ' ' >= window->font->first_char && ' ' <= window->font->last_char) {
                int space_index = ' ' - window->font->first_char;
                space_width = window->font_size.value * window->font->metrics[space_index * 7 + 6];
            } else {
                space_width = window->font_size.value * 3; // Default space width
            }
        }
        
        float total_width = word_width + space_width;

        // Check if adding this COMPLETE word would exceed the line width
        if (current_line_width + total_width > available_width && current_line_width > 0.0f) {
            // Start a new line
            dest[dest_pos++] = '\n';
            current_line_width = 0.0f;
        }

        // Add only the visible characters (typed so far)
        for (size_t i = 0; i < visible_length && dest_pos < sizeof(window->wrapped_text) - 1; i++) {
            dest[dest_pos++] = source[word_start + i];
        }

        // Calculate actual width of what we added (visible portion only)
        float visible_width = calculate_word_width(window, &source[word_start], visible_length);
        current_line_width += visible_width;

        // Add trailing space only if we've typed past the word
        if (has_trailing_space && visible_end > word_end) {
            if (dest_pos < sizeof(window->wrapped_text) - 1) {
                dest[dest_pos++] = ' ';
                current_line_width += space_width;
            }
            source_pos = word_end + 1;  // Skip past the space
        } else {
            source_pos = visible_end;
            // Special case: if we're sitting on a space (zero-length word)
            // and we've typed past it, add the space and advance
            if (source_pos == word_start && has_trailing_space && max_chars > word_end) {
                if (dest_pos < sizeof(window->wrapped_text) - 1 && current_line_width > 0.0f) {
                    dest[dest_pos++] = ' ';
                    current_line_width += space_width;
                }
                source_pos = word_end + 1;
            }
        }
    }
    
    dest[dest_pos] = '\0';
    
    // Check if scrolling is needed
    int current_lines = count_lines_in_text(window->wrapped_text);
    int max_lines = calculate_max_visible_lines(window);
    
    // If we have more lines than can fit, scroll up
    while (current_lines > max_lines && current_lines > 1) {
        scroll_text_up_one_line(window->wrapped_text);
        current_lines--;
    }
}

// Count the number of lines in text (counting \n characters + 1)
int count_lines_in_text(const char* text) {
    if (!text || *text == '\0') return 0;
    
    int line_count = 1; // Start with 1 since first line doesn't need \n
    const char* ptr = text;
    
    while (*ptr) {
        if (*ptr == '\n') {
            line_count++;
        }
        ptr++;
    }
    
    return line_count;
}

// Calculate maximum number of lines that can fit in the window
int calculate_max_visible_lines(Window* window) {
    if (!window->font) return 1;
    
    // Available height for text
    float available_height = window->size.height - window->text_margins.top - window->text_margins.bottom;
    
    // Line height calculation
    float line_height = window->font_size.value * window->font->height * window->line_spacing.value;
    
    // Calculate how many lines fit
    int max_lines = (int)(available_height / line_height);
    
    // Ensure at least 1 line
    return max_lines > 0 ? max_lines : 1;
}

// Remove the first line from text by shifting everything up
void scroll_text_up_one_line(char* text) {
    if (!text || *text == '\0') return;
    
    // Find the first newline
    char* first_newline = strchr(text, '\n');
    if (!first_newline) {
        // No newline found, clear entire text
        *text = '\0';
        return;
    }
    
    // Move everything after the first newline to the beginning
    char* src = first_newline + 1; // Start after the newline
    char* dst = text; // Start at beginning
    
    // Shift text left
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0'; // Null terminate
}

void wm_update(WindowManager* wm, DeltaTime delta_time) {
    if (!wm) return;
    
    for (int i = 0; i < wm->window_count; i++) {
        Window* window = &wm->windows[i];
        
        if (!window->visible || !window->auto_typing) continue;
        
        size_t text_len = strlen(window->full_text);
        if (window->typing_position.position >= text_len) continue;
        
        // Auto-typing logic
        window->typing_timer += delta_time.seconds;
        
        float chars_per_second = window->typing_speed.chars_per_second;
        if (window->typing_timer >= (1.0f / chars_per_second)) {
            window->typing_timer = 0.0f;
            
            // Advance typing position
            window->typing_position.position++;
            
            // Wrap text with proper line breaks
            wrap_text_to_window(window);
        }
    }
}

void wm_render(WindowManager* wm) {
    if (!wm) return;
    
    // Setup projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, wm->screen_size.width, wm->screen_size.height, 0, -1, 1);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // Render all visible windows
    for (int i = 0; i < wm->window_count; i++) {
        Window* window = &wm->windows[i];
        if (!window->visible) continue;
        
        ScreenCoord pos = wm_calculate_position(wm, window);
        
        // Render 9-slice border using the texture
        if (window->nine_slice.texture.id > 0) {
            render_nine_slice_border(
                window->nine_slice.texture.id,
                pos.x, pos.y,
                window->size.width, window->size.height,
                window->border_size.size,
                window->center_alpha
            );
        } else {
            // Fallback to simple rectangle if 9-slice failed to load
            glColor4f(0.2f, 0.3f, 0.4f, 0.9f);
            glBegin(GL_QUADS);
            glVertex2f(pos.x, pos.y);
            glVertex2f(pos.x + window->size.width, pos.y);
            glVertex2f(pos.x + window->size.width, pos.y + window->size.height);
            glVertex2f(pos.x, pos.y + window->size.height);
            glEnd();
        }
        
        // Render text or menu items
        if (window->font) {
            // Check if this is an outline font (fonts with pre-rendered colors)
            // Outline fonts need white color so the texture colors show through correctly
            bool is_outline_font = (strstr(window->font_path.value, "outline") != NULL);

            if (is_outline_font) {
                // Use white color for outline fonts to preserve texture colors (especially white centers)
                glColor4f(1.0f, 1.0f, 1.0f, window->text_color.a);
            } else {
                // Use window's text color for regular fonts
                glColor4f(window->text_color.r, window->text_color.g,
                          window->text_color.b, window->text_color.a);
            }

            // Check if this is a menu window (has menu items)
            if (window->menu_item_count > 0) {
                // Render menu items in a column
                float y_offset = pos.y + window->text_margins.top;
                float line_height = window->font_size.value * 16.0f * (1.0f + window->line_spacing.value);

                // Render menu header if present
                if (strlen(window->menu_header) > 0) {
                    // Use header font if available, otherwise use regular font
                    FontInfo* header_font = window->menu_header_font ? window->menu_header_font : window->font;

                    if (header_font) {
                        // Calculate header line height using header font size
                        float header_line_height = window->menu_header_font_size.value * 16.0f * (1.0f + window->line_spacing.value);

                        // Render background box for header (only in content area, not over borders)
                        if (window->menu_header_bg_color.a > 0.0f) {
                            // Background box starts at text margin left, ends at text margin right
                            // Extended by padding values on all sides
                            float bg_x = pos.x + window->text_margins.left - window->menu_header_bg_padding_x;
                            float bg_y = y_offset - window->menu_header_bg_padding_y;
                            float bg_width = window->size.width - window->text_margins.left - window->text_margins.right
                                            + (window->menu_header_bg_padding_x * 2);
                            float bg_height = header_line_height + (window->menu_header_bg_padding_y * 2);

                            glDisable(GL_TEXTURE_2D);
                            glColor4f(window->menu_header_bg_color.r, window->menu_header_bg_color.g,
                                      window->menu_header_bg_color.b, window->menu_header_bg_color.a);
                            glBegin(GL_QUADS);
                            glVertex2f(bg_x, bg_y);
                            glVertex2f(bg_x + bg_width, bg_y);
                            glVertex2f(bg_x + bg_width, bg_y + bg_height);
                            glVertex2f(bg_x, bg_y + bg_height);
                            glEnd();
                        }

                        // Check if header font is an outline font
                        bool is_header_outline_font = false;
                        if (window->menu_header_font && strlen(window->menu_header_font_path.value) > 0) {
                            is_header_outline_font = (strstr(window->menu_header_font_path.value, "outline") != NULL);
                        }

                        // Set color for header text (white for outline fonts)
                        if (is_header_outline_font) {
                            glColor4f(1.0f, 1.0f, 1.0f, window->text_color.a);
                        } else {
                            // Use window's text color for regular header fonts
                            glColor4f(window->text_color.r, window->text_color.g,
                                      window->text_color.b, window->text_color.a);
                        }

                        // Measure text width for centering using header font size
                        float text_width = wm_measure_text_width(header_font, window->menu_header, window->menu_header_font_size);
                        float available_width = window->size.width - window->text_margins.left - window->text_margins.right;
                        float center_offset = (available_width - text_width) / 2.0f;

                        // Ensure minimum left margin
                        if (center_offset < 0) center_offset = 0;

                        RenderText(header_font, window->menu_header,
                                  pos.x + window->text_margins.left + center_offset,
                                  y_offset,
                                  window->menu_header_font_size.value,
                                  window->line_spacing.value);

                        // Restore color for menu items (check if regular font is outline)
                        bool is_outline_font = (strstr(window->font_path.value, "outline") != NULL);
                        if (is_outline_font) {
                            glColor4f(1.0f, 1.0f, 1.0f, window->text_color.a);
                        } else {
                            glColor4f(window->text_color.r, window->text_color.g,
                                      window->text_color.b, window->text_color.a);
                        }

                        y_offset += header_line_height * 1.5f;  // Extra spacing after header
                    }
                }

                // Render menu items
                for (int j = 0; j < window->menu_item_count; j++) {
                    RenderText(window->font, window->menu_items[j],
                              pos.x + window->text_margins.left,
                              y_offset,
                              window->font_size.value,
                              window->line_spacing.value);
                    y_offset += line_height;
                }
            } else if (strlen(window->wrapped_text) > 0) {
                // Render regular text content
                RenderText(window->font, window->wrapped_text,
                          pos.x + window->text_margins.left,
                          pos.y + window->text_margins.top,
                          window->font_size.value,
                          window->line_spacing.value);
            }
        }
    }
}

// Dummy implementations for functions that aren't needed for basic demo
void wm_trigger_event(WindowManager* wm, EventName event_name) {
    (void)wm; (void)event_name;
}

void wm_handle_key(WindowManager* wm, int key, int action) {
    (void)wm; (void)key; (void)action;
}

void wm_handle_char(WindowManager* wm, unsigned int codepoint) {
    (void)wm; (void)codepoint;
}

void wm_handle_mouse(WindowManager* wm, double x, double y, int button, int action) {
    (void)wm; (void)x; (void)y; (void)button; (void)action;
}

void wm_cleanup(WindowManager* wm) {
    if (!wm) return;

    // Track freed fonts to avoid double-free when multiple windows share the same font
    FontInfo* freed_fonts[MAX_WINDOWS] = {NULL};
    int freed_count = 0;

    // Clean up any allocated resources
    for (int i = 0; i < wm->window_count; i++) {
        Window* window = &wm->windows[i];
        if (window->font) {
            // Check if this font has already been freed
            bool already_freed = false;
            for (int j = 0; j < freed_count; j++) {
                if (freed_fonts[j] == window->font) {
                    already_freed = true;
                    break;
                }
            }

            // Only free if not already freed
            if (!already_freed) {
                FreeFont(window->font);
                freed_fonts[freed_count++] = window->font;
            }
            window->font = NULL;
        }
    }

    printf("✓ Window manager cleanup complete\n");
}