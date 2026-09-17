#include <string.h>
#include "usb_profile.h"

// Keep the already deployed Windows identity, topology and interface numbers.
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x40a4,
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1
};
#define AC_ENTITY_BYTES (8 + 17 + 18 + 12 + 17 + 12)
#define CONFIG_BYTES (9 + 8 + 9 + 9 + AC_ENTITY_BYTES + 62 + 55)
#define PROFILE(interval) \
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_BYTES, 0, 100), \
    TUD_AUDIO_DESC_IAD(0, 3, 0), \
    TUD_AUDIO_DESC_STD_AC(0, 0, 4), \
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_PRO_AUDIO, AC_ENTITY_BYTES, 0), \
    /* External fixed Seed clock; frequency R, validity R, not SOF-synchronised. */ \
    TUD_AUDIO_DESC_CLK_SRC(4, 0, 5, 0, 0), \
    TUD_AUDIO_DESC_INPUT_TERM(1, AUDIO_TERM_TYPE_USB_STREAMING, 0, 4, 2, 3, 0, 0, 0), \
    /* Preserve feature unit 2 and its mute/volume control capabilities. */ \
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(2, 1, 15, 15, 15, 0), \
    TUD_AUDIO_DESC_OUTPUT_TERM(3, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0, 2, 4, 0, 0), \
    TUD_AUDIO_DESC_INPUT_TERM(0x11, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x13, 4, 2, 3, 0, 0, 0), \
    TUD_AUDIO_DESC_OUTPUT_TERM(0x13, AUDIO_TERM_TYPE_USB_STREAMING, 0, 0x11, 4, 0, 0), \
    TUD_AUDIO_DESC_STD_AS_INT(1, 0, 0, 5), \
    TUD_AUDIO_DESC_STD_AS_INT(1, 1, 2, 5), \
    TUD_AUDIO_DESC_CS_AS_INT(1, 0, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, 2, 3, 0), \
    TUD_AUDIO_DESC_TYPE_I_FORMAT(4, 24), \
    TUD_AUDIO_DESC_STD_AS_ISO_EP(UAC_PLAY_EP, 5, UAC_MAX_BYTES, interval), \
    TUD_AUDIO_DESC_CS_AS_ISO_EP(0, 0, 0, 0), \
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(UAC_FB_EP, 4, interval), \
    TUD_AUDIO_DESC_STD_AS_INT(2, 0, 0, 6), \
    TUD_AUDIO_DESC_STD_AS_INT(2, 1, 1, 6), \
    TUD_AUDIO_DESC_CS_AS_INT(0x13, 0, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, 2, 3, 0), \
    TUD_AUDIO_DESC_TYPE_I_FORMAT(4, 24), \
    TUD_AUDIO_DESC_STD_AS_ISO_EP(UAC_CAP_EP, 5, UAC_MAX_BYTES, interval), \
    TUD_AUDIO_DESC_CS_AS_ISO_EP(0, 0, 0, 0)

static const uint8_t hs[] = { PROFILE(4) };
static const uint8_t fs[] = { PROFILE(1) };
_Static_assert(sizeof(hs) == CONFIG_BYTES, "UAC2 descriptor length");
static uint8_t other[CONFIG_BYTES];
static const tusb_desc_device_qualifier_t qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t), .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64, .bNumConfigurations = 1
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    return index ? NULL : (tud_speed_get() == TUSB_SPEED_HIGH ? hs : fs);
}
const uint8_t *tud_descriptor_device_qualifier_cb(void) { return (const uint8_t *)&qualifier; }
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
    if (index) return NULL;
    memcpy(other, tud_speed_get() == TUSB_SPEED_HIGH ? fs : hs, sizeof(other));
    other[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return other;
}
const tusb_desc_endpoint_t *uac_endpoint(uint8_t ep) {
    const uint8_t *p = tud_descriptor_configuration_cb(0);
    for (size_t n = 0; n < sizeof(hs); n += p[n]) {
        if (p[n + 1] == TUSB_DESC_ENDPOINT && p[n + 2] == ep)
            return (const tusb_desc_endpoint_t *)(p + n);
    }
    return NULL;
}
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t result[32];
    static const char *const strings[] = {
        "", "Seed3 P4 Lab", "Seed3 P4 SPI UAC2 2x2", "SEED3P4SPI2",
        "usb uac", "speaker", "microphone"
    };
    if (index >= sizeof(strings) / sizeof(strings[0])) return NULL;
    unsigned n = 1;
    if (!index) result[1] = 0x0409;
    else {
        n = strlen(strings[index]);
        if (n > 31) n = 31;
        for (unsigned i = 0; i < n; ++i) result[i + 1] = strings[index][i];
    }
    result[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return result;
}
