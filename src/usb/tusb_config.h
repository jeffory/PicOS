#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// Enable Device stack
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED

// Default role mode
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#endif

// Can't use #include "tusb_option.h" safely without ensuring path,
// usually TinyUSB includes it anyway.
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------
#define CFG_TUD_ENDPOINT0_SIZE 64

// Interfaces: 1 CDC (requires 2 interfaces: control + data), 1 MSC
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// CDC FIFO size
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256

// MSC Buffer size of Device Mass storage
#define CFG_TUD_MSC_EP_BUFSIZE 512

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
