#include "seed_audio_clock.h"
#include "spi_audio_protocol.h"
#include "stm32h7xx_hal.h"

bool SeedConfigureAudioClock(daisy::DaisySeed& seed, uint32_t rate)
{
    if(!spi_audio_rate_is_supported(rate)) return false;
    const uint32_t family = (rate == 44100U || rate == 88200U) ? 44100U : 48000U;
    RCC_PeriphCLKInitTypeDef clocks{};
    HAL_RCCEx_GetPeriphCLKConfig(&clocks);
    // Same PLL3 divider tree as the board, adjusted with the fractional PLL.
    // Kernel clock is 1024 * family; SAI selects the 48/96 kHz divider family.
    const uint64_t numerator = uint64_t(family) * 1024U
                              * clocks.PLL3.PLL3M * clocks.PLL3.PLL3P * 8192U;
    const uint64_t multiplier = (numerator + HSE_VALUE / 2U) / HSE_VALUE;
    clocks.PLL3.PLL3N = multiplier / 8192U;
    clocks.PLL3.PLL3FRACN = multiplier % 8192U;
    clocks.PeriphClockSelection = RCC_PERIPHCLK_SAI1;
    clocks.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLL3;
    if(HAL_RCCEx_PeriphCLKConfig(&clocks) != HAL_OK) return false;
    seed.SetAudioSampleRate(rate >= 88200U
        ? daisy::SaiHandle::Config::SampleRate::SAI_96KHZ
        : daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    // libDaisy has no 44.1/88.2 enum. HAL rounds MCKDIV down unless the
    // fractional digit is >8: using the 48k enum with a 45.1584 MHz kernel
    // would incorrectly select /3 (58.8k) instead of /4 (44.1k).
    // Audio is stopped, so set the exact integer divider for our PLL tree.
    if((SAI1_Block_A->CR1 | SAI1_Block_B->CR1) & SAI_xCR1_SAIEN) return false;
    const uint32_t divider = rate >= 88200U ? 2U : 4U;
    MODIFY_REG(SAI1_Block_A->CR1, SAI_xCR1_MCKDIV, divider << SAI_xCR1_MCKDIV_Pos);
    MODIFY_REG(SAI1_Block_B->CR1, SAI_xCR1_MCKDIV, divider << SAI_xCR1_MCKDIV_Pos);
    return true;
}
