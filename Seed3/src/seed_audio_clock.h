#pragma once
#include "daisy_seed.h"
#include <cstdint>

// Call ONLY with audio stopped. This project owns PLL3 (no other ADC/I2C4/SAI2
// consumers). libDaisy itself is left unchanged; use the explicit rate in DSP,
// not DaisySeed::AudioSampleRate(), which reports the enum's 48/96 kHz base.
bool SeedConfigureAudioClock(daisy::DaisySeed& seed, uint32_t rate);
