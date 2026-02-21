#include "pico/unique_id.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // Use Miscellaneous class for composite devices
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0x2E8A,  // Raspberry Pi
    .idProduct = 0x000A, // Standard Pico SDK USB product ID
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_TOTAL };

// endpoints
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82

#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

#define CONFIG_TOTAL_LEN                                                       \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute,
    // power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // CDC
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),

    // MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index; // unused
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// Array of pointer to string descriptors
char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "Raspberry Pi",             // 1: Manufacturer
    "PicOS Device",             // 2: Product
    "123456",                   // 3: Serials (will be replaced by read_uid)
    "PicOS CDC",                // 4: Interface CDC
    "PicOS MSC",                // 5: Interface MSC
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long
// enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  uint8_t chr_count;

  if (index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

    if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
      return NULL;

    const char *str = string_desc_arr[index];

    // Read Pico Unique ID for serial
    pico_unique_board_id_t board_id;
    char serial_str[17];
    if (index == 3) {
      pico_get_unique_board_id(&board_id);
      for (int i = 0; i < 8; i++) {
        sprintf(&serial_str[i * 2], "%02X", board_id.id[i]);
      }
      str = serial_str;
    }

    // Cap at max char
    chr_count = (uint8_t)strlen(str);
    if (chr_count > 31)
      chr_count = 31;

    // Convert ASCII string into UTF-16
    for (uint8_t i = 0; i < chr_count; i++) {
      _desc_str[1 + i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

  return _desc_str;
}
