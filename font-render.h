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
    int tex_width;   // Cached texture width (avoids expensive glGet calls)
    int tex_height;  // Cached texture height (avoids expensive glGet calls)

    // VBO support for batched rendering (OPTIMIZATION)
    GLuint vbo;      // Vertex buffer object
    GLuint vao;      // Unused in legacy mode, kept for struct compatibility
    float* vertex_buffer;  // CPU-side buffer for building batches
    int vertex_buffer_capacity;  // Max vertices (in floats)
} FontInfo;

// Initializes the font rendering engine, loads a font and its texture.
FontInfo* LoadFont(const char* json_filename, const char* texture_filename);

// Frees the memory associated with a loaded font.
void FreeFont(FontInfo* font_info);

// Renders a single character using the specified font, position, and size.
void RenderCharacter(GLuint texture, FontInfo *font_info, char c, float x, float y, float font_size);

// Renders a string of text using the specified font, position, and size.
void RenderText(FontInfo *font_info, const char* text, float x, float y, float font_size, float line_spacing);

// Renders a string of text tinted to the given RGBA colour.
// Sets the current GL colour as a side effect; callers that also draw
// uncoloured text must restore it afterwards.
void RenderTextColored(FontInfo *font_info, const char* text, float x, float y,
                       float font_size, float line_spacing,
                       float r, float g, float b, float a);

// Get the average character width for this font (useful for terminal layout)
float GetAverageCharWidth(FontInfo *font_info, float font_size);

#endif // FONT_RENDERER_H
