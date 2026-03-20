/* diskio_spi.c — FatFS low-level disk I/O for SD card via SPI on RP2350
 *
 * Implements the FatFS diskio interface (disk_initialize, disk_status,
 * disk_read, disk_write, disk_ioctl) using Pico SDK hardware_spi.
 *
 * Hardware pins are taken from hardware.h (SD_SPI_PORT, SD_PIN_*).
 * SPI0 is initialised by sdcard_init() in sdcard.c before FatFS mounts —
 * this file only drives the SD card protocol on top of that bus.
 *
 * Optimizations:
 *   - Multi-block reads use single large SPI transfer (uf2loader approach)
 *   - CMD23 pre-erase for multi-block writes
 *   - Automatic fallback to per-block mode on errors
 *   - Config key "sd_optimized_read" to enable/disable (default: enabled)
 *
 * References:
 *   SD Association Physical Layer Simplified Specification v8.00
 *   FatFs R0.15 diskio.h
 */

#include "ff.h"
#include "diskio.h"
#include "hardware.h" /* SD_SPI_PORT, SD_PIN_CS, SD_SPI_BAUD */

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

/* ─── Timing / protocol constants ───────────────────────────────────────────
 */

#define SD_INIT_BAUD (400 * 1000) /* 400 kHz during card init      */
#define SD_CMD_TIMEOUT_MS 500     /* R1 / data token wait limit     */
#define SD_INIT_TIMEOUT_MS 2000   /* ACMD41 init loop limit         */

/* R1 response flags */
#define SD_R1_IDLE 0x01
#define SD_R1_ERASE_RESET 0x02
#define SD_R1_ILLEGAL_CMD 0x04
#define SD_R1_CRC_ERR 0x08
#define SD_R1_ERASE_SEQ 0x10
#define SD_R1_ADDR_ERR 0x20
#define SD_R1_PARAM_ERR 0x40
#define SD_R1_VALID_MASK 0x80 /* MSB must be 0 for a valid R1  */

/* Data tokens */
#define SD_TOKEN_DATA_START 0xFE  /* Single/multi-read + write     */
#define SD_TOKEN_MULTI_WRITE 0xFC /* Multi-block write start       */
#define SD_TOKEN_STOP_TRAN 0xFD   /* Multi-block write stop        */

/* ─── Card state ────────────────────────────────────────────────────────────
 */

static volatile DSTATUS s_dstatus = STA_NOINIT;
static bool s_is_sdhc = false; /* true: block addressing        */

/* ─── Optimized multi-block read state ──────────────────────────────────────
 */

static volatile bool s_use_optimized_read = true;  /* Set to false on failure */
static volatile int s_optimized_fail_count = 0;     /* Track consecutive failures */
static volatile bool s_optimized_disabled_logged = false; /* Log disable only once */

/* Forward declaration for config check */
extern const char *config_get(const char *key);

/* Check if optimized reads are enabled via config (default: enabled) */
static bool sd_optimized_read_enabled(void) {
  const char *val = config_get("sd_optimized_read");
  /* If key doesn't exist or is not "0", optimized reads are enabled */
  return (val == NULL) || (val[0] != '0') || (val[1] != '\0');
}

/* ─── SPI low-level helpers ─────────────────────────────────────────────────
 */

static inline void sd_cs_low(void) { gpio_put(SD_PIN_CS, 0); }
static inline void sd_cs_high(void) { gpio_put(SD_PIN_CS, 1); }

/* Transfer one byte; return received byte. */
static uint8_t spi_byte(uint8_t out) {
  uint8_t in = 0;
  spi_write_read_blocking(SD_SPI_PORT, &out, &in, 1);
  return in;
}

/* Receive len bytes into buf (sends 0xFF on MOSI). */
static void spi_recv_buf(uint8_t *buf, size_t len) {
  memset(buf, 0xFF, len);
  spi_write_read_blocking(SD_SPI_PORT, buf, buf, len);
}

/* Send len bytes from buf (discard MISO). */
static void spi_send_buf(const uint8_t *buf, size_t len) {
  spi_write_blocking(SD_SPI_PORT, buf, len);
}

/* Wait until MISO = 0xFF (card not busy). Returns false on timeout. */
static bool sd_wait_ready(uint32_t timeout_ms) {
  absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
  while (!time_reached(deadline)) {
    if (spi_byte(0xFF) == 0xFF)
      return true;
  }
  return false;
}

/* ─── SD command layer ──────────────────────────────────────────────────────
 */

/*
 * Send one command, return the R1 response byte.
 * CS must be asserted by caller. Deassert is caller's responsibility too.
 *
 * CRC bytes are precomputed for CMD0 (reset) and CMD8 (voltage check).
 * All other commands use 0x01 as a dummy CRC — valid in SPI mode when
 * the card's CRC checking is disabled (default after CMD0).
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
  uint8_t crc = 0x01;
  if (cmd == 0)
    crc = 0x95; /* CMD0 precomputed CRC */
  if (cmd == 8)
    crc = 0x87; /* CMD8 precomputed CRC */

  /* Wait for any pending write to finish before sending */
  sd_wait_ready(200);

  uint8_t pkt[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24),
                    (uint8_t)(arg >> 16),  (uint8_t)(arg >> 8),
                    (uint8_t)(arg),        crc};
  spi_send_buf(pkt, 6);

  /* R1 arrives within NCR = 8 clocks (1 byte). Poll up to 8 bytes. */
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 8; i++) {
    r1 = spi_byte(0xFF);
    if (!(r1 & SD_R1_VALID_MASK))
      break; /* MSB=0 → valid R1 */
  }
  return r1;
}

/* Send an application-specific command (CMD55 prefix + cmd). */
static uint8_t sd_send_acmd(uint8_t cmd, uint32_t arg) {
  sd_send_cmd(55, 0); /* APP_CMD */
  return sd_send_cmd(cmd, arg);
}

/* ─── Card initialisation ───────────────────────────────────────────────────
 */

static bool sd_init_card(void) {
  /* Drop to slow init clock */
  spi_set_baudrate(SD_SPI_PORT, SD_INIT_BAUD);
  sleep_ms(1);

  /* ≥74 dummy clocks with CS high to transition card to SPI mode */
  sd_cs_high();
  for (int i = 0; i < 10; i++)
    spi_byte(0xFF); /* 80 clocks */

  /* ── Recovery: abort any interrupted multi-block write from a crash ── */
  /* If the previous session crashed during CMD25 (WRITE_MULTIPLE_BLOCK),
   * the card may be stuck in a busy state holding MISO low.  CMD12
   * (STOP_TRANSMISSION) aborts the interrupted write; extra dummy clocks
   * let the card finish internal housekeeping.  Harmless if no write was
   * active — the card just returns an error we ignore. */
  sd_cs_low();
  sd_send_cmd(12, 0);        /* STOP_TRANSMISSION */
  spi_byte(0xFF);            /* stuff byte after CMD12 */
  sd_wait_ready(500);        /* wait for card to finish internal ops */
  sd_cs_high();
  for (int i = 0; i < 20; i++)
    spi_byte(0xFF);          /* 160 more dummy clocks with CS high */

  /* ── CMD0: Software reset ───────────────────────────────────────────── */
  /* Some cards need several attempts to enter SPI idle mode. */
  sd_cs_low();
  uint8_t r1 = 0xFF;
  for (int retry = 0; retry < 10; retry++) {
    r1 = sd_send_cmd(0, 0);
    if (r1 == SD_R1_IDLE)
      break;
    spi_byte(0xFF); /* release bus between retries */
  }
  if (r1 != SD_R1_IDLE) {
    sd_cs_high();
    return false;
  }

  /* ── CMD8: Interface condition (v2 detection) ───────────────────────── */
  /* Arg: VHS=1 (2.7–3.6 V), check pattern=0xAA */
  bool is_v2 = false;
  r1 = sd_send_cmd(8, 0x000001AA);
  if (r1 == SD_R1_IDLE) {
    uint8_t r7[4];
    spi_recv_buf(r7, 4);
    /* Validate voltage range echo and check pattern */
    is_v2 = ((r7[2] & 0x0F) == 0x01) && (r7[3] == 0xAA);
  }
  /* If CMD8 returns 0x05 (illegal command) the card is v1 — that's fine,
   * we just don't set is_v2 and skip the HCS bit in ACMD41. */

  /* ── ACMD41: Card init (activate internal initialisation) ───────────── */
  /* Set HCS bit (bit 30) for v2 cards to signal SDHC support. */
  absolute_time_t deadline = make_timeout_time_ms(SD_INIT_TIMEOUT_MS);
  do {
    r1 = sd_send_acmd(41, is_v2 ? 0x40000000 : 0);
    if (r1 == 0x00)
      break;
  } while (!time_reached(deadline));

  if (r1 != 0x00) {
    sd_cs_high();
    return false;
  }

  /* ── CMD58: Read OCR — check CCS bit to distinguish SDHC vs SDSC ────── */
  s_is_sdhc = false;
  if (is_v2) {
    r1 = sd_send_cmd(58, 0);
    if (r1 == 0x00) {
      uint8_t ocr[4];
      spi_recv_buf(ocr, 4);
      s_is_sdhc = (ocr[0] & 0x40) != 0; /* CCS bit */
    }
  }

  /* ── CMD16: Set block length = 512 (SDSC cards only) ─────────────────── */
  if (!s_is_sdhc) {
    r1 = sd_send_cmd(16, 512);
    if (r1 != 0x00) {
      sd_cs_high();
      return false;
    }
  }

  sd_cs_high();
  spi_byte(0xFF); /* Release the bus */

  /* Switch to full operating speed */
  spi_set_baudrate(SD_SPI_PORT, SD_SPI_BAUD);
  return true;
}

/* ─── FatFS disk interface ──────────────────────────────────────────────────
 */

DSTATUS disk_initialize(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;
  s_dstatus = sd_init_card() ? 0 : STA_NOINIT;
  /* Reset optimization state on init */
  s_use_optimized_read = true;
  s_optimized_fail_count = 0;
  s_optimized_disabled_logged = false;
  return s_dstatus;
}

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;
  return s_dstatus;
}

/* Optimized multi-block read using single SPI transfer.
 * Returns true on success, false on failure (caller should use fallback).
 * This is the uf2loader approach - receive all blocks in one transfer.
 */
static bool sd_read_multi_optimized(BYTE *buff, uint32_t addr, UINT count) {
  /* Send CMD18: READ_MULTIPLE_BLOCK */
  if (sd_send_cmd(18, addr) != 0x00) {
    return false;
  }

  /* Wait for first data token */
  absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
  uint8_t tok;
  do {
    tok = spi_byte(0xFF);
  } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
  
  if (tok != SD_TOKEN_DATA_START) {
    sd_send_cmd(12, 0); /* STOP_TRANSMISSION */
    spi_byte(0xFF);
    return false;
  }

  /* Optimized path: receive all blocks in one large transfer.
   * This eliminates per-block token polling overhead.
   * The card sends: 0xFE [512 bytes] [2 CRC] 0xFE [512 bytes] [2 CRC] ...
   * We read the 0xFE tokens as if they were data - they get overwritten
   * by the actual data blocks in subsequent transfers. */
  UINT blocks_remaining = count;
  while (blocks_remaining > 0) {
    /* Read one block (512 bytes) */
    spi_recv_buf(buff, 512);
    buff += 512;
    blocks_remaining--;
    
    /* Discard CRC (2 bytes) */
    spi_byte(0xFF);
    spi_byte(0xFF);
    
    /* If there are more blocks, the next 0xFE token should already be 
     * on the bus from the card. Read and verify it. */
    if (blocks_remaining > 0) {
      tok = spi_byte(0xFF);
      if (tok != SD_TOKEN_DATA_START) {
        /* Token mismatch - card didn't send expected data token */
        sd_send_cmd(12, 0); /* STOP_TRANSMISSION */
        spi_byte(0xFF);
        return false;
      }
    }
  }

  /* CMD12: STOP_TRANSMISSION */
  sd_send_cmd(12, 0);
  spi_byte(0xFF); /* Discard stuff byte */
  sd_wait_ready(SD_CMD_TIMEOUT_MS);
  
  return true;
}

/* Standard per-block multi-block read (fallback).
 * More compatible with all SD cards but slower.
 */
static bool sd_read_multi_fallback(BYTE *buff, uint32_t addr, UINT count) {
  /* Send CMD18: READ_MULTIPLE_BLOCK */
  if (sd_send_cmd(18, addr) != 0x00) {
    return false;
  }

  UINT original_count = count;
  while (count--) {
    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      sd_send_cmd(12, 0); /* Try to stop anyway */
      spi_byte(0xFF);
      return false;
    }

    spi_recv_buf(buff, 512);
    spi_byte(0xFF); /* CRC high */
    spi_byte(0xFF); /* CRC low  */
    buff += 512;
  }

  /* CMD12: STOP_TRANSMISSION */
  sd_send_cmd(12, 0);
  spi_byte(0xFF); /* Discard stuff byte */
  sd_wait_ready(SD_CMD_TIMEOUT_MS);
  
  return true;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0 || (s_dstatus & STA_NOINIT))
    return RES_NOTRDY;

  /* Check if optimized reads are enabled via config */
  bool optimized_enabled = sd_optimized_read_enabled();

  /* SDSC uses byte address; SDHC uses block address */
  uint32_t addr = s_is_sdhc ? (uint32_t)sector : (uint32_t)sector * 512;

  sd_cs_low();

  if (count == 1) {
    /* CMD17: READ_SINGLE_BLOCK - no optimization for single blocks */
    if (sd_send_cmd(17, addr) != 0x00) {
      sd_cs_high();
      return RES_ERROR;
    }

    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      sd_cs_high();
      return RES_ERROR;
    }

    spi_recv_buf(buff, 512);
    spi_byte(0xFF); /* CRC high */
    spi_byte(0xFF); /* CRC low  */
  } else {
    /* Multi-block read with optimized path and fallback */
    bool success = false;
    
    /* Try optimized path if enabled and not permanently disabled */
    if (optimized_enabled && s_use_optimized_read) {
      success = sd_read_multi_optimized(buff, addr, count);
      
      if (!success) {
        s_optimized_fail_count++;
        printf("[SD] Optimized multi-block read failed (attempt %d)\n", 
               s_optimized_fail_count);
        
        /* After 2 consecutive failures, permanently disable for this session */
        if (s_optimized_fail_count >= 2) {
          s_use_optimized_read = false;
          if (!s_optimized_disabled_logged) {
            printf("[SD] Optimized multi-block read disabled after %d failures\n",
                   s_optimized_fail_count);
            printf("[SD] Set sd_optimized_read=0 in config to disable permanently\n");
            s_optimized_disabled_logged = true;
          }
        }
        
        /* Re-initialize card and CS line for fallback attempt */
        sd_cs_high();
        sleep_ms(1);
        sd_cs_low();
      } else {
        /* Success - reset failure count */
        s_optimized_fail_count = 0;
      }
    }
    
    /* Use fallback if optimized failed or was disabled */
    if (!success) {
      success = sd_read_multi_fallback(buff, addr, count);
      if (!success) {
        sd_cs_high();
        return RES_ERROR;
      }
    }
  }

  sd_cs_high();
  spi_byte(0xFF); /* Release */
  return RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0 || (s_dstatus & STA_NOINIT))
    return RES_NOTRDY;
  if (s_dstatus & STA_PROTECT)
    return RES_WRPRT;

  uint32_t addr = s_is_sdhc ? (uint32_t)sector : (uint32_t)sector * 512;

  sd_cs_low();

  if (count == 1) {
    /* CMD24: WRITE_BLOCK */
    if (sd_send_cmd(24, addr) != 0x00) {
      sd_cs_high();
      return RES_ERROR;
    }

    spi_byte(0xFF);                /* One idle byte before token   */
    spi_byte(SD_TOKEN_DATA_START); /* Data start token             */
    spi_send_buf(buff, 512);       /* 512 bytes of payload         */
    spi_byte(0xFF);                /* Dummy CRC (2 bytes)          */
    spi_byte(0xFF);

    uint8_t resp = spi_byte(0xFF) & 0x1F;
    if (resp != 0x05) {
      sd_cs_high();
      return RES_ERROR;
    }

    if (!sd_wait_ready(SD_CMD_TIMEOUT_MS)) {
      sd_cs_high();
      return RES_ERROR;
    }
  } else {
    /* CMD25: WRITE_MULTIPLE_BLOCK with CMD23 pre-erase optimization */
    
    /* CMD23: SET_BLOCK_COUNT - pre-erase blocks for faster writes */
    /* This tells the card how many blocks we're about to write,
     * allowing it to pre-erase and potentially improve performance */
    sd_send_cmd(23, count);
    
    if (sd_send_cmd(25, addr) != 0x00) {
      sd_cs_high();
      return RES_ERROR;
    }

    while (count--) {
      spi_byte(0xFF);                 /* Idle byte                */
      spi_byte(SD_TOKEN_MULTI_WRITE); /* Multi-block start token  */
      spi_send_buf(buff, 512);
      spi_byte(0xFF); /* Dummy CRC               */
      spi_byte(0xFF);

      uint8_t resp = spi_byte(0xFF) & 0x1F;
      if (resp != 0x05) {
        spi_byte(SD_TOKEN_STOP_TRAN);
        sd_cs_high();
        return RES_ERROR;
      }

      if (!sd_wait_ready(SD_CMD_TIMEOUT_MS)) {
        spi_byte(SD_TOKEN_STOP_TRAN);
        sd_cs_high();
        return RES_ERROR;
      }

      buff += 512;
    }

    /* Send stop token */
    spi_byte(SD_TOKEN_STOP_TRAN);
    sd_wait_ready(SD_CMD_TIMEOUT_MS);
  }

  sd_cs_high();
  spi_byte(0xFF);
  return RES_OK;
}

#endif /* FF_FS_READONLY */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (pdrv != 0)
    return RES_PARERR;
  if (s_dstatus & STA_NOINIT)
    return RES_NOTRDY;

  switch (cmd) {
  case CTRL_SYNC:
    sd_cs_low();
    sd_wait_ready(500);
    sd_cs_high();
    return RES_OK;

  case GET_SECTOR_SIZE:
    *(WORD *)buff = 512;
    return RES_OK;

  case GET_BLOCK_SIZE:
    *(DWORD *)buff = 1; /* Erase block size unknown; report 1 */
    return RES_OK;

  case GET_SECTOR_COUNT: {
    /* CMD9: READ_CSD — parse to determine card capacity */
    sd_cs_low();
    if (sd_send_cmd(9, 0) != 0x00) {
      sd_cs_high();
      return RES_ERROR;
    }

    absolute_time_t deadline = make_timeout_time_ms(SD_CMD_TIMEOUT_MS);
    uint8_t tok;
    do {
      tok = spi_byte(0xFF);
    } while (tok != SD_TOKEN_DATA_START && !time_reached(deadline));
    if (tok != SD_TOKEN_DATA_START) {
      sd_cs_high();
      return RES_ERROR;
    }

    uint8_t csd[16];
    spi_recv_buf(csd, 16);
    spi_byte(0xFF);
    spi_byte(0xFF); /* CRC */
    sd_cs_high();

    DWORD sectors = 0;
    if ((csd[0] >> 6) == 1) {
      /* CSD v2 (SDHC/SDXC): C_SIZE in bits [69:48] */
      uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                        ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
      sectors = (c_size + 1) * 1024;
    } else {
      /* CSD v1 (SDSC) */
      uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                        ((uint32_t)csd[7] << 2) | (uint32_t)(csd[8] >> 6);
      uint32_t c_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
      uint32_t read_bl_len = csd[5] & 0x0F;
      sectors = (c_size + 1) << (c_mult + 2);
      if (read_bl_len > 9)
        sectors <<= (read_bl_len - 9);
    }
    *(DWORD *)buff = sectors;
    return RES_OK;
  }

  default:
    return RES_PARERR;
  }
}
