#include "image_decoders.h"
#include <AnimatedGIF.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <tgx.h>

extern "C" {
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
}

// --- FatFS Proxy Callbacks for decoders ---

static void *my_file_open(const char *szFilename, int32_t *pFileSize) {
  sdfile_t f = sdcard_fopen(szFilename, "rb");
  if (!f)
    return NULL;
  int size = sdcard_fsize(szFilename);
  if (size < 0) {
    sdcard_fclose(f);
    return NULL;
  }
  if (pFileSize)
    *pFileSize = size;
  return (void *)f;
}

static void my_file_close(void *pHandle) {
  if (pHandle) {
    sdcard_fclose((sdfile_t)pHandle);
  }
}

// JPEG Proxy
static int32_t my_jpeg_read(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_jpeg_seek(JPEGFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

// PNG Proxy
static int32_t my_png_read(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_png_seek(PNGFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

// GIF Proxy
static int32_t my_gif_read(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_gif_seek(GIFFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

static int my_JPEGDraw(JPEGDRAW *pDraw) {
  return tgx::JPEGDraw<JPEGDEC, JPEGDRAW>(pDraw);
}

static int my_PNGDraw(PNGDRAW *pDraw) {
  tgx::PNGDraw<PNG, PNGDRAW>(pDraw);
  return 1;
}

static void my_GIFDraw(GIFDRAW *pDraw) {
  tgx::GIFDraw<AnimatedGIF, GIFDRAW>(pDraw);
}

bool decode_jpeg_buffer(const uint8_t *data, size_t len,
                        image_decode_result_t *result) {
  if (!data || len == 0 || !result) {
    printf("[TGX] Invalid arguments to decode_jpeg_buffer\n");
    return false;
  }

  printf("[TGX] Decoding JPEG, buffer size: %zu bytes\n", len);

  JPEGDEC *jpeg = (JPEGDEC *)umm_malloc(sizeof(JPEGDEC));
  if (!jpeg) {
    printf("[TGX] Failed to allocate JPEGDEC object\n");
    return false;
  }

  if (jpeg->openRAM((uint8_t *)data, (int)len, my_JPEGDraw)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();
    size_t req_mem = w * h * sizeof(uint16_t);
    printf("[TGX] JPEG openRAM success. Dimensions: %dx%d. Requesting %zu "
           "bytes from PSRAM.\n",
           w, h, req_mem);

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      int dec_res = im.JPEGDecode(*jpeg, {0, 0}, 0);
      printf("[TGX] JPEGDecode result: %d\n", dec_res);
      jpeg->close();
      umm_free(jpeg);
      return true;
    } else {
      printf("[TGX] PSRAM OOM allocating %zu bytes for image data\n", req_mem);
    }
    jpeg->close();
  } else {
    printf("[TGX] JPEG openRAM failed with error: %d\n", jpeg->getLastError());
  }
  umm_free(jpeg);
  return false;
}

bool decode_png_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result) {
  if (!data || len == 0 || !result)
    return false;

  PNG *png = (PNG *)umm_malloc(sizeof(PNG));
  if (!png)
    return false;

  if (png->openRAM((uint8_t *)data, (int)len, my_PNGDraw)) {
    int w = png->getWidth();
    int h = png->getHeight();
    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(w * h * sizeof(uint16_t));

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.PNGDecode(*png, {0, 0});
      png->close();
      umm_free(png);
      return true;
    }
    png->close();
  }
  umm_free(png);
  return false;
}

bool decode_gif_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result) {
  if (!data || len == 0 || !result)
    return false;

  AnimatedGIF *gif = (AnimatedGIF *)umm_malloc(sizeof(AnimatedGIF));
  if (!gif)
    return false;

  if (gif->open((uint8_t *)data, (int)len, my_GIFDraw)) {
    int w = gif->getCanvasWidth();
    int h = gif->getCanvasHeight();
    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(w * h * sizeof(uint16_t));

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.GIFplayFrame(*gif, {0, 0});
      gif->close();
      umm_free(gif);
      return true;
    }
    gif->close();
  }
  umm_free(gif);
  return false;
}

bool decode_jpeg_file(const char *path, image_decode_result_t *result) {
  if (!path || !result) {
    printf("[TGX] Invalid arguments to decode_jpeg_file\n");
    return false;
  }

  printf("[TGX] Decoding JPEG from file: %s\n", path);

  JPEGDEC *jpeg = (JPEGDEC *)umm_malloc(sizeof(JPEGDEC));
  if (!jpeg) {
    printf("[TGX] Failed to allocate JPEGDEC object\n");
    return false;
  }

  if (jpeg->open(path, my_file_open, my_file_close, my_jpeg_read, my_jpeg_seek,
                 my_JPEGDraw)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();

    int scale_div = 1;
    int scale_opt = 0;

    // Use JPEG sub-sampling to natively shrink 4K/8K images without fully
    // decoding them into PSRAM! We target a maximum of ~1-2 megapixel buffer
    // footprints
    while ((size_t)(w / scale_div) * (size_t)(h / scale_div) > 1000000 &&
           scale_div < 8) {
      if (scale_div == 1) {
        scale_div = 2;
        scale_opt = 2; /* JPEG_SCALE_HALF */
      } else if (scale_div == 2) {
        scale_div = 4;
        scale_opt = 4; /* JPEG_SCALE_QUARTER */
      } else if (scale_div == 4) {
        scale_div = 8;
        scale_opt = 8; /* JPEG_SCALE_EIGHTH */
      }
    }

    int out_w = w / scale_div;
    int out_h = h / scale_div;
    size_t req_mem = out_w * out_h * sizeof(uint16_t);

    printf("[TGX] JPEG open success. Original: %dx%d. Downscaled 1/%d: %dx%d. "
           "Requesting %zu bytes.\n",
           w, h, scale_div, out_w, out_h, req_mem);

    result->w = out_w;
    result->h = out_h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, out_w, out_h);
      im.clear(tgx::RGB565_Black);
      int dec_res = im.JPEGDecode(*jpeg, {0, 0}, scale_opt);
      printf("[TGX] JPEGDecode result: %d\n", dec_res);
      jpeg->close();
      umm_free(jpeg);
      return true;
    } else {
      printf("[TGX] PSRAM OOM allocating %zu bytes for image data\n", req_mem);
    }
    jpeg->close();
  } else {
    printf("[TGX] JPEG file open failed with error: %d\n",
           jpeg->getLastError());
  }
  umm_free(jpeg);
  return false;
}

bool decode_png_file(const char *path, image_decode_result_t *result) {
  if (!path || !result) {
    return false;
  }

  PNG *png = (PNG *)umm_malloc(sizeof(PNG));
  if (!png) {
    return false;
  }

  if (png->open(path, my_file_open, my_file_close, my_png_read, my_png_seek,
                my_PNGDraw)) {
    int w = png->getWidth();
    int h = png->getHeight();
    size_t req_mem = w * h * sizeof(uint16_t);

    if (req_mem > 4000000) {
      printf("[TGX] Image too large! PNG cannot be hardware downscaled: %zu "
             "bytes\n",
             req_mem);
      png->close();
      umm_free(png);
      return false;
    }

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.PNGDecode(*png, {0, 0});
      png->close();
      umm_free(png);
      return true;
    }
    png->close();
  }
  umm_free(png);
  return false;
}

bool decode_gif_file(const char *path, image_decode_result_t *result) {
  if (!path || !result) {
    return false;
  }

  AnimatedGIF *gif = (AnimatedGIF *)umm_malloc(sizeof(AnimatedGIF));
  if (!gif) {
    return false;
  }

  if (gif->open(path, my_file_open, my_file_close, my_gif_read, my_gif_seek,
                my_GIFDraw)) {
    int w = gif->getCanvasWidth();
    int h = gif->getCanvasHeight();
    size_t req_mem = w * h * sizeof(uint16_t);

    if (req_mem > 4000000) {
      printf("[TGX] Image too large! GIF cannot be hardware downscaled: %zu "
             "bytes\n",
             req_mem);
      gif->close();
      umm_free(gif);
      return false;
    }

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.GIFplayFrame(*gif, {0, 0});
      gif->close();
      umm_free(gif);
      return true;
    }
    gif->close();
  }
  umm_free(gif);
  return false;
}

extern "C" void tgx_draw_image_scaled(uint16_t *dst_fb, int dst_w, int dst_h,
                                      const uint16_t *src_data, int src_w,
                                      int src_h, int dst_x, int dst_y,
                                      float scale, float angle) {
  if (!dst_fb || !src_data)
    return;

  tgx::Image<tgx::RGB565> dst_im(dst_fb, dst_w, dst_h);
  // Since tgx::Image requires non-const pointer for its constructor, we cast
  // away const. The blitScaledRotated method takes the source image by value or
  // const reference, so it won't modify the source pixels.
  tgx::Image<tgx::RGB565> src_im((uint16_t *)src_data, src_w, src_h);

  // Anchor at the center of the source image to draw it at the (dst_x, dst_y)
  // center point
  dst_im.blitScaledRotated(src_im, {src_w / 2.0f, src_h / 2.0f},
                           {(float)dst_x, (float)dst_y}, scale, angle);
}

extern "C" void tgx_draw_image_scaled_masked(uint16_t *dst_fb, int dst_w, int dst_h,
                                            const uint16_t *src_data, int src_w,
                                            int src_h, int dst_x, int dst_y,
                                            float scale, float angle,
                                            uint16_t transparent_color) {
  if (!dst_fb || !src_data)
    return;

  tgx::Image<tgx::RGB565> dst_im(dst_fb, dst_w, dst_h);
  tgx::Image<tgx::RGB565> src_im((uint16_t *)src_data, src_w, src_h);

  // Anchor at the center of the source image to draw it at the (dst_x, dst_y)
  // center point, with transparency
  tgx::RGB565 mask_color(transparent_color);
  dst_im.blitScaledRotatedMasked(src_im, mask_color,
                                 {src_w / 2.0f, src_h / 2.0f},
                                 {(float)dst_x, (float)dst_y}, scale, angle);
}
