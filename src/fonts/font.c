#include "font.h"
#include <string.h>

int font_glyph_width(const pc_font_t *f, unsigned char c) {
  if (c < f->first || c > f->last) return f->max_width;
  return f->widths ? f->widths[c - f->first] : f->max_width;
}

int font_text_width(const pc_font_t *f, const char *text) {
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    w += font_glyph_width(f, *p);
  return w;
}

int font_wrap_line(const pc_font_t *f, const char *text, int max_w) {
  const unsigned char *p = (const unsigned char *)text;
  int n = 0, w = 0, last_space = -1;
  while (p[n] && p[n] != '\n') {
    if (p[n] == ' ') last_space = n;
    int gw = font_glyph_width(f, p[n]);
    if (w + gw > max_w) break;
    w += gw;
    n++;
  }
  bool overflowed = p[n] && p[n] != '\n';
  if (overflowed && last_space > 0) n = last_space;
  if (n == 0 && p[0] && p[0] != '\n') n = 1;
  return n;
}

// Blit one glyph cell. `on_at(row, col)` semantics are inlined for the two
// shapes we have (bitmap glyph, fallback box) so the clip and color logic
// exists exactly once.
static void blit_cell(uint16_t *buf, int buf_w,
                      int cx0, int cy0, int cx1, int cy1,
                      int x, int y, int w, int h,
                      const uint8_t *rows, int stride,   // NULL => fallback box
                      uint16_t fg, uint16_t bg, bool transparent) {
  for (int row = 0; row < h; row++) {
    int py = y + row;
    if (py < cy0 || py > cy1) continue;
    uint16_t *dst = buf + (size_t)py * buf_w;
    const uint8_t *bits = rows ? rows + row * stride : NULL;
    for (int col = 0; col < w; col++) {
      int px = x + col;
      if (px < cx0 || px > cx1) continue;
      bool on;
      if (bits) {
        on = (bits[col >> 3] & (0x80 >> (col & 7))) != 0;
      } else {
        // Hollow box occupying rows 0..h-2 and cols 0..w-2 of the cell.
        on = row < h - 1 && col < w - 1 &&
             (row == 0 || row == h - 2 || col == 0 || col == w - 2);
      }
      if (on) dst[px] = fg;
      else if (!transparent) dst[px] = bg;
    }
  }
}

int font_render(const pc_font_t *f, uint16_t *buf, int buf_w,
                int cx0, int cy0, int cx1, int cy1,
                int x, int y, const char *text,
                uint16_t fg, uint16_t bg, bool transparent) {
  int start_x = x;
  int h = f->height;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    unsigned char c = *p;
    if (c < f->first || c > f->last) {
      int adv = f->max_width;
      blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, x, y, adv, h,
                NULL, 0, fg, bg, transparent);
      x += adv;
      continue;
    }
    int gi = c - f->first;
    // Deliberately inlines font_glyph_width: the glyph index is needed anyway
    // for the bitmap pointer, so re-deriving it inside a call would be waste.
    int adv = f->widths ? f->widths[gi] : f->max_width;
    const uint8_t *glyph = f->bitmaps + (size_t)gi * h * f->stride;
    blit_cell(buf, buf_w, cx0, cy0, cx1, cy1, x, y, adv, h,
              glyph, f->stride, fg, bg, transparent);
    x += adv;
  }
  return x - start_x;
}

bool font_from_blob(pc_font_t *out, const void *blob, size_t len) {
  const uint8_t *b = (const uint8_t *)blob;
  if (!out || !b || len < PFNT_HEADER_SIZE) return false;
  if (memcmp(b, "PFNT", 4) != 0) return false;
  if (b[4] != PFNT_VERSION) return false;
  if (b[5] & ~PFNT_FLAG_PROPORTIONAL) return false;   // unknown flag bits
  uint8_t first = b[6], last = b[7], height = b[8], max_w = b[9], stride = b[10];
  if (last < first) return false;
  if (height < 1 || height > FONT_MAX_DIM) return false;
  if (max_w < 1 || max_w > FONT_MAX_DIM) return false;
  if (stride != (max_w + 7) / 8) return false;
  if (b[11] != 0) return false;
  size_t count = (size_t)last - first + 1;
  size_t expect = PFNT_HEADER_SIZE + count + count * height * stride;
  if (len != expect) return false;
  const uint8_t *widths = b + PFNT_HEADER_SIZE;
  for (size_t i = 0; i < count; i++)
    if (widths[i] < 1 || widths[i] > max_w) return false;
  out->first = first;
  out->last = last;
  out->height = height;
  out->max_width = max_w;
  out->stride = stride;
  out->widths = widths;
  out->bitmaps = widths + count;
  out->blob = NULL;
  return true;
}
