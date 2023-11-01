//
// Created by Kurosu Chan on 2023/11/1.
//

#ifndef BLE_LORA_ADAPTER_COMMON_H
#define BLE_LORA_ADAPTER_COMMON_H

#include <driver/gpio.h>

namespace common {
namespace pin {
  // https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/gpio.html
  constexpr auto SCK  = GPIO_NUM_8;
  constexpr auto MOSI = GPIO_NUM_9;
  constexpr auto MISO = GPIO_NUM_10;
  // i.e. NSS chip select
  constexpr auto CS   = GPIO_NUM_3;
  constexpr auto BUSY = GPIO_NUM_19;
  constexpr auto RST  = GPIO_NUM_18;
  constexpr auto DIO1 = GPIO_NUM_1;
  constexpr auto DIO2 = GPIO_NUM_2;
}
static const char *BLE_CHAR_HR_SERVICE_UUID     = "180d";
static const char *BLE_CHAR_HEARTBEAT_UUID      = "048b8928-d0a5-43e2-ada9-b925ec62ba27";
static const char *BLE_CHAR_DEVICE_UUID         = "12a481f0-9384-413d-b002-f8660566d3b0";
static const char *BLE_STANDARD_HR_SERVICE_UUID = "180d";
static const char *BLE_STANDARD_HR_CHAR_UUID    = "2a37";
constexpr auto BLE_NAME                         = "LoRA-Adapter";
constexpr auto SCAN_TIME                        = std::chrono::milliseconds(750);
// scan time + sleep time
constexpr auto SCAN_TOTAL_TIME = std::chrono::milliseconds(1000);
static_assert(SCAN_TOTAL_TIME > SCAN_TIME);
}

#endif // BLE_LORA_ADAPTER_COMMON_H