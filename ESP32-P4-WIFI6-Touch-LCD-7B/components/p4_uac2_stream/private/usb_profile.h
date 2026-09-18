#pragma once
#include "tusb.h"
#include "class/audio/audio.h"
enum { UAC_PORT = 1, UAC_PLAY_EP = 0x01, UAC_FB_EP = 0x81, UAC_CAP_EP = 0x82,
       UAC_PLAY_ITF = 1, UAC_CAP_ITF = 2, UAC_MAX_FRAMES = 49,
       UAC_STREAM_ALT = 1, UAC_VALID_BITS = 24, UAC_SUBSLOT_BYTES = 3,
       UAC_FRAME_BYTES = 2 * UAC_SUBSLOT_BYTES,
       UAC_MAX_BYTES = UAC_MAX_FRAMES * UAC_FRAME_BYTES };
const tusb_desc_endpoint_t *uac_endpoint(uint8_t ep, uint8_t alternate);
