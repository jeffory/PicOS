#pragma once
// Shared bitmap font description and renderer. Pure: no display, SD card
// or allocator dependencies, so firmware, simulator and host unit tests all
// compile this one file.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PFNT_HEADER_SIZE        12
#define PFNT_VERSION            1
#define PFNT_FLAG_PROPORTIONAL  0x01
#define FONT_MAX_DIM            64

typedef struct {
  uint8_t first, last;        // byte codes covered, inclusive
  uint8_t height;             // line height in px, 1..64
  uint8_t max_width;          // widest advance, 1..64; the "cell" width
  uint8_t stride;             // bytes per glyph row = (max_width + 7) / 8
  const uint8_t *widths;      // advance per glyph; NULL => all = max_width
  const uint8_t *bitmaps;     // row-major, MSB = leftmost; height*stride per glyph
  void *blob;                 // umm_malloc'd file image (loaded), NULL (built-in)
} pc_font_t;

static inline int font_glyph_count(const pc_font_t *f) {
  return (int)f->last - (int)f->first + 1;
}

// Advance of one byte. Bytes outside first..last advance max_width.
int font_glyph_width(const pc_font_t *f, unsigned char c);

// Sum of advances for a NUL-terminated string.
int font_text_width(const pc_font_t *f, const char *text);

// Number of bytes of `text` that fit within max_w px. Stops before '\n'.
// When the line overflows and a space was seen after position 0, breaks
// at that space (the space itself is not counted). Returns at least 1
// when *text is neither NUL nor '\n', so callers always make progress.
int font_wrap_line(const pc_font_t *f, const char *text, int max_w);

// Draw text into a 16-bit buffer of row pitch buf_w. Only pixels inside
// the inclusive clip rect [cx0..cx1] x [cy0..cy1] are written. Colors are
// stored as given (callers pre-swap for the byte-swapped hardware
// framebuffer). `transparent` skips background writes. Bytes outside
// first..last draw a hollow box. Returns the total advance in px.
int font_render(const pc_font_t *f, uint16_t *buf, int buf_w,
                int cx0, int cy0, int cx1, int cy1,
                int x, int y, const char *text,
                uint16_t fg, uint16_t bg, bool transparent);

// Validate a .pfn image and point `out` at its tables. No copy, no
// ownership: `out->blob` is left NULL for the caller to fill in. Every
// width must be 1..max_width. Returns false on any violation.
bool font_from_blob(pc_font_t *out, const void *blob, size_t len);
