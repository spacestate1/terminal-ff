#ifndef FONT_RENDERER_H
#define FONT_RENDERER_H

#include <GL/glew.h>
#include <stdbool.h>

typedef struct {
    int height;
    int first_char;
    int last_char;
    int *metrics;
    int metrics_count;
    int max_descent;
    GLuint texture;
} FontInfo;

// Initializes the font rendering engine, loads a font and its texture.
FontInfo* LoadFont(const char* json_filename, const char* texture_filename);

// Frees the memory associated with a loaded font.
void FreeFont(FontInfo* font_info);

// Renders a single character using the specified font, position, and size.
void RenderCharacter(GLuint texture, FontInfo *font_info, char c, float x, float y, float font_size);

// Renders a string of text using the specified font, position, and size.
void RenderText(FontInfo *font_info, const char* text, float x, float y, float font_size, float line_spacing);

// Get the average character width for this font (useful for terminal layout)
float GetAverageCharWidth(FontInfo *font_info, float font_size);

#endif // FONT_RENDERER_H
