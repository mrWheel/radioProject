#pragma once
#include "sdkconfig.h"
#define RADIO_I2S_BCLK     CONFIG_RADIO_I2S_BCLK_GPIO
#define RADIO_I2S_WS       CONFIG_RADIO_I2S_WS_GPIO
#define RADIO_I2S_DOUT     CONFIG_RADIO_I2S_DOUT_GPIO
#define RADIO_I2S_ENABLE   CONFIG_RADIO_I2S_ENABLE_GPIO
//-- Kconfig bool options are only #defined when set to "y" (undefined, not
//-- 0, when "n"), so a plain CONFIG_RADIO_ENCODER_REVERSED reference outside
//-- an #if fails to compile. This gives callers a always-defined 0/1 value.
#if CONFIG_RADIO_ENCODER_REVERSED
#define RADIO_ENCODER_REVERSED_DEFAULT 1
#else
#define RADIO_ENCODER_REVERSED_DEFAULT 0
#endif

