#include <GL/glew.h>  // Add this first
#include "font-render.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// STB_IMAGE_IMPLEMENTATION is defined in terminal.c (only one implementation unit)
#include "stb_image.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

// Function to load font information from a JSON file
FontInfo* LoadFontInfo(const char* filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open font info file: %s\n", filename);
        perror("Error");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *json_text = malloc(file_size + 1);
    if (!json_text) {
        printf("Failed to allocate memory for JSON text\n");
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(json_text, 1, file_size, file);
    json_text[bytes_read] = '\0';
    fclose(file);

    unsigned int tokens_capacity = 1 + file_size / 2;
    json_token_t *tokens = malloc(tokens_capacity * sizeof(json_token_t));
    if (!tokens) {
        printf("Failed to allocate memory for JSON tokens\n");
        free(json_text);
        return NULL;
    }

    unsigned int parsed_size_req = 0;
    int tokens_len = json_tokenize(json_text, bytes_read, tokens, tokens_capacity, &parsed_size_req);
    if (tokens_len <= 0) {
        printf("JSON tokenization failed with error: %d\n", tokens_len);
        free(json_text);
        free(tokens);
        return NULL;
    }

    json_t *root = malloc(parsed_size_req);
    if (!root) {
        printf("Failed to allocate memory for parsed JSON\n");
        free(json_text);
        free(tokens);
        return NULL;
    }

    json_parse_tokens(json_text, tokens, tokens_len, root);

    FontInfo *font_info = malloc(sizeof(FontInfo));
    if (!font_info) {
        printf("Failed to allocate memory for FontInfo\n");
        free(json_text);
        free(tokens);
        free(root);
        return NULL;
    }

    font_info->height = (int)json_number(json_value_for_key(root, "height"));
    font_info->first_char = (int)json_number(json_value_for_key(root, "first_char"));
    font_info->last_char = (int)json_number(json_value_for_key(root, "last_char"));

    json_t *metrics = json_value_for_key(root, "metrics");
    if (!metrics || metrics->type != JSON_ARRAY) {
        printf("Invalid or missing metrics in JSON\n");
        free(json_text);
        free(tokens);
        free(root);
        free(font_info);
        return NULL;
    }

    font_info->metrics_count = metrics->len;
    font_info->metrics = malloc(font_info->metrics_count * sizeof(int));
    if (!font_info->metrics) {
        printf("Failed to allocate memory for metrics\n");
        free(json_text);
        free(tokens);
        free(root);
        free(font_info);
        return NULL;
    }

    for (int i = 0; i < font_info->metrics_count; i++) {
        font_info->metrics[i] = (int)json_number(json_value_at(metrics, i));
    }

    // Validate metrics count matches expected glyph count
    int glyph_count = font_info->last_char - font_info->first_char + 1;
    if (font_info->metrics_count != glyph_count * 7) {
        fprintf(stderr, "Font metrics size mismatch: expected %d (glyphs=%d * 7), got %d\n",
                glyph_count * 7, glyph_count, font_info->metrics_count);
        free(json_text);
        free(tokens);
        free(root);
        free(font_info->metrics);
        free(font_info);
        return NULL;
    }

    // Calculate max_descent
    font_info->max_descent = 0;
    for (int i = 5; i < font_info->metrics_count; i += 7) {
        int descent = font_info->metrics[i] + font_info->metrics[i - 2];
        if (descent > font_info->max_descent) {
            font_info->max_descent = descent;
        }
    }

    free(json_text);
    free(tokens);
    free(root);

    return font_info;
}

// Function to free font information
void FreeFontInfo(FontInfo* font_info) {
    if (font_info) {
        free(font_info->metrics);
        free(font_info);
    }
}

// Function to load a texture from a file
GLuint LoadTexture(const char* filename, bool useMipMaps, GLuint wrapflag, bool pixelate) {
    //printf("Loading image from %s...\n", filename);

    // DO NOT flip fonts - font JSON coordinates are designed for non-flipped textures
    stbi_set_flip_vertically_on_load(0);

    int width, height, components;
    unsigned char* data = stbi_load(filename, &width, &height, &components, 4);
    if (!data) {
        printf("Failed to load image: %s\n", filename);
        printf("stbi_failure_reason: %s\n", stbi_failure_reason());
        return 0;
    }

    //printf("Image loaded successfully. Width: %d, Height: %d, Components: %d\n", width, height, components);

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    if (useMipMaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pixelate ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, useMipMaps ? GL_LINEAR_MIPMAP_LINEAR : (pixelate ? GL_NEAREST : GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapflag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapflag);

    stbi_image_free(data);

    return textureID;
}

// Function to load a font
FontInfo* LoadFont(const char* json_filename, const char* texture_filename) {
    FontInfo* font_info = LoadFontInfo(json_filename);
    if (!font_info) {
        return NULL;
    }

    font_info->texture = LoadTexture(texture_filename, false, GL_CLAMP_TO_EDGE, true);
    if (!font_info->texture) {
        FreeFont(font_info);
        return NULL;
    }

    // OPTIMIZATION: Cache texture dimensions to avoid expensive glGet calls during rendering
    int width, height;
    glBindTexture(GL_TEXTURE_2D, font_info->texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    font_info->tex_width = width;
    font_info->tex_height = height;

    // OPTIMIZATION: Initialize VBO for batched rendering
    // Allocate buffer for ~10,000 characters (enough for large terminal)
    // Each char = 6 vertices (2 triangles), each vertex = 4 floats (x, y, u, v)
    font_info->vertex_buffer_capacity = 10000 * 6 * 4;
    font_info->vertex_buffer = malloc(font_info->vertex_buffer_capacity * sizeof(float));
    if (!font_info->vertex_buffer) {
        fprintf(stderr, "Failed to allocate vertex buffer\n");
        FreeFont(font_info);
        return NULL;
    }

    // Generate VBO (no VAO needed for legacy OpenGL)
    glGenBuffers(1, &font_info->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, font_info->vbo);

    // Allocate GPU buffer (dynamic draw pattern)
    glBufferData(GL_ARRAY_BUFFER,
                 font_info->vertex_buffer_capacity * sizeof(float),
                 NULL, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Note: VAO not used in legacy OpenGL mode
    font_info->vao = 0;

    return font_info;
}

// Function to free font information
void FreeFont(FontInfo* font_info) {
    if (font_info) {
        free(font_info->metrics);
        glDeleteTextures(1, &font_info->texture);

        // Clean up VBO resources
        if (font_info->vbo) glDeleteBuffers(1, &font_info->vbo);
        if (font_info->vertex_buffer) free(font_info->vertex_buffer);

        free(font_info);
    }
}
//Text is properly aligned but everything is upaide down and mirroed the wrong direction

void RenderCharacter(GLuint texture, FontInfo *font_info, char c, float x, float y, float font_size) {
    int char_index = c - font_info->first_char;
    int glyph_count = font_info->last_char - font_info->first_char + 1;

    if (char_index < 0 || char_index >= glyph_count) {
        printf("Character '%c' (ASCII %d) not in font range\n", c, (int)c);
        return;
    }

    int metric_index = char_index * 7;
    // Defensive bounds check before accessing metrics array
    if (metric_index + 6 >= font_info->metrics_count) {
        printf("Character '%c' metric index %d out of bounds (metrics_count=%d)\n",
               c, metric_index, font_info->metrics_count);
        return;
    }

    int tex_x = font_info->metrics[metric_index];
    int tex_y = font_info->metrics[metric_index + 1];
    int width = font_info->metrics[metric_index + 2];
    int height = font_info->metrics[metric_index + 3];
    int offset_x = font_info->metrics[metric_index + 4];
    int offset_y = font_info->metrics[metric_index + 5];
    // int advance_x = font_info->metrics[metric_index + 6];  // Commented out as unused

    // OPTIMIZATION: Use cached texture dimensions instead of expensive glGet calls
    float tex_width = (float)font_info->tex_width;
    float tex_height = (float)font_info->tex_height;

    // Improved baseline calculation for consistent vertical alignment
    float char_x = x + offset_x * font_size;

    // The baseline is at y position. We need to:
    // 1. Move up from y by the font's ascent (height - max_descent)
    // 2. Adjust by the character's specific offset_y
    float baseline = y;
    float char_y = baseline + offset_y * font_size;

    float u1 = (float)tex_x / tex_width;
    float v1 = (float)tex_y / tex_height;
    float u2 = (float)(tex_x + width) / tex_width;
    float v2 = (float)(tex_y + height) / tex_height;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBegin(GL_QUADS);
    // Using the corrected texture coordinates for proper orientation
    glTexCoord2f(u1, v1); glVertex2f(char_x, char_y);
    glTexCoord2f(u2, v1); glVertex2f(char_x + width * font_size, char_y);
    glTexCoord2f(u2, v2); glVertex2f(char_x + width * font_size, char_y + height * font_size);
    glTexCoord2f(u1, v2); glVertex2f(char_x, char_y + height * font_size);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}


// OPTIMIZED: Batched rendering using VBO - renders all characters in one draw call.
// Draws with whatever the current GL colour is; the wrappers below set it.
static void render_text_batch(FontInfo *font_info, const char* text, float x, float y, float font_size, float line_spacing) {
    if (!font_info || !text || !font_info->vertex_buffer) return;

    float start_x = x;
    float line_height = font_size * font_info->height * line_spacing;
    float tex_width = (float)font_info->tex_width;
    float tex_height = (float)font_info->tex_height;
    int glyph_count = font_info->last_char - font_info->first_char + 1;

    // Build vertex buffer for all characters
    int vertex_count = 0;
    float* vbuf = font_info->vertex_buffer;
    int max_vertices = font_info->vertex_buffer_capacity;

    while (*text) {
        if (*text == '\n') {
            x = start_x;
            y += line_height;
            text++;
            continue;
        }

        // Check if character is in range
        int char_index = *text - font_info->first_char;
        if (char_index < 0 || char_index >= glyph_count) {
            // Try replacement character
            char_index = '?' - font_info->first_char;
            if (char_index < 0 || char_index >= glyph_count) {
                x += font_size * 5; // Default advance
                text++;
                continue;
            }
        }

        int metric_index = char_index * 7;
        if (metric_index + 6 >= font_info->metrics_count) {
            text++;
            continue;
        }

        // Get character metrics
        int tex_x = font_info->metrics[metric_index];
        int tex_y = font_info->metrics[metric_index + 1];
        int width = font_info->metrics[metric_index + 2];
        int height = font_info->metrics[metric_index + 3];
        int offset_x = font_info->metrics[metric_index + 4];
        int offset_y = font_info->metrics[metric_index + 5];
        int advance_x = font_info->metrics[metric_index + 6];

        // Calculate character position
        float char_x = x + offset_x * font_size;
        float char_y = y + offset_y * font_size;
        float char_w = width * font_size;
        float char_h = height * font_size;

        // Calculate texture coordinates
        float u1 = (float)tex_x / tex_width;
        float v1 = (float)tex_y / tex_height;
        float u2 = (float)(tex_x + width) / tex_width;
        float v2 = (float)(tex_y + height) / tex_height;

        // Check buffer capacity (6 vertices * 4 floats per char)
        if (vertex_count + 24 > max_vertices) {
            break; // Buffer full
        }

        // Triangle 1 (top-left, top-right, bottom-left)
        vbuf[vertex_count++] = char_x;           vbuf[vertex_count++] = char_y;
        vbuf[vertex_count++] = u1;               vbuf[vertex_count++] = v1;

        vbuf[vertex_count++] = char_x + char_w; vbuf[vertex_count++] = char_y;
        vbuf[vertex_count++] = u2;               vbuf[vertex_count++] = v1;

        vbuf[vertex_count++] = char_x;           vbuf[vertex_count++] = char_y + char_h;
        vbuf[vertex_count++] = u1;               vbuf[vertex_count++] = v2;

        // Triangle 2 (top-right, bottom-right, bottom-left)
        vbuf[vertex_count++] = char_x + char_w; vbuf[vertex_count++] = char_y;
        vbuf[vertex_count++] = u2;               vbuf[vertex_count++] = v1;

        vbuf[vertex_count++] = char_x + char_w; vbuf[vertex_count++] = char_y + char_h;
        vbuf[vertex_count++] = u2;               vbuf[vertex_count++] = v2;

        vbuf[vertex_count++] = char_x;           vbuf[vertex_count++] = char_y + char_h;
        vbuf[vertex_count++] = u1;               vbuf[vertex_count++] = v2;

        x += font_size * advance_x;
        text++;
    }

    // If no vertices, nothing to draw
    if (vertex_count == 0) return;

    // Enable OpenGL state
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, font_info->texture);

    // Upload vertices and draw using legacy client states (compatible with existing OpenGL context)
    glBindBuffer(GL_ARRAY_BUFFER, font_info->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_count * sizeof(float), vbuf);

    // Enable client states for legacy OpenGL
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    // Set pointers (stride = 4 floats: x, y, u, v)
    glVertexPointer(2, GL_FLOAT, 4 * sizeof(float), (void*)0);
    glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    // Draw in ONE call
    glDrawArrays(GL_TRIANGLES, 0, vertex_count / 4);

    // Cleanup
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

void RenderText(FontInfo *font_info, const char* text, float x, float y, float font_size, float line_spacing) {
    render_text_batch(font_info, text, x, y, font_size, line_spacing);
}

// Same as RenderText but tints the batch. Sets the current GL colour as a side
// effect - callers mixing coloured and uncoloured text must restore it.
void RenderTextColored(FontInfo *font_info, const char* text, float x, float y,
                       float font_size, float line_spacing,
                       float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    render_text_batch(font_info, text, x, y, font_size, line_spacing);
}

// Get average character width for terminal layout calculations
float GetAverageCharWidth(FontInfo *font_info, float font_size) {
    if (!font_info || !font_info->metrics) {
        return font_size * 10.0f; // Fallback
    }

    // For monospace fonts, all characters should have the same width
    // Get the advance_x for 'M' which is typically representative
    char test_char = 'M';
    int char_index = test_char - font_info->first_char;
    int glyph_count = font_info->last_char - font_info->first_char + 1;

    if (char_index < 0 || char_index >= glyph_count) {
        // Try space character as fallback
        test_char = ' ';
        char_index = test_char - font_info->first_char;
    }

    if (char_index >= 0 && char_index < glyph_count) {
        int metric_index = char_index * 7;
        // Defensive bounds check before accessing metrics array
        if (metric_index + 6 >= font_info->metrics_count) {
            return font_size * 10.0f; // Fallback if out of bounds
        }
        int advance_x = font_info->metrics[metric_index + 6];
        return font_size * (float)advance_x;
    }

    // Fallback to estimate
    return font_size * 10.0f;
}

