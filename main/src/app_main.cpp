#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "scan_manager.h"
#include "server_callback.h"
#include "whitelist_char_callback.h"
#include "esp_hal.h"
#include "common.h"
#include "hr_lora.h"
#include "app_nvs.h"
#include <endian.h>

extern "C" void app_main();

bool RxFlag = false;

void app_main() {
  using namespace common;
  using namespace blue;
  const auto TAG = "main";
  ESP_LOGI(TAG, "boot");

  auto err = app_nvs::nvs_init();
  ESP_ERROR_CHECK(err);
  app_nvs::addr_t addr{0};
  bool has_addr = false;
  err           = app_nvs::get_addr(&addr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "no device addr, fallback back to nullptr; reason %s (%d);", esp_err_to_name(err), err);
  } else {
    ESP_LOGI(TAG, "addr=%s", utils::toHex(addr.data(), addr.size()).c_str());
    has_addr = true;
  }

  /**
   * @brief a key that is used to map the name of the device to a number
   */
  auto *name_map_key = new uint8_t(0);
  err                = app_nvs::get_name_map_key(name_map_key);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "no name map key, fallback back to 0; reason %s (%d);", esp_err_to_name(err), err);
  } else {
    ESP_LOGI(TAG, "name map key=%d", *name_map_key);
  }

  auto &hal = *new ESPHal(pin::SCK, pin::MISO, pin::MOSI);
  hal.init();
  ESP_LOGI(TAG, "hal init success!");
  auto &module = *new Module(&hal, pin::CS, pin::DIO1, pin::RST, pin::BUSY);
  auto &rf     = *new LLCC68(&module);
  auto st      = rf.begin(434, 500.0, 7, 7,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                          22, 8, 1.6);
  if (st != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "failed, code %d", st);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
  }
  ESP_LOGI(TAG, "RF began!");
  rf.setPacketReceivedAction([]() {
    RxFlag = true;
  });
  rf.standby();
  rf.startReceive();

  NimBLEDevice::init(BLE_NAME);
  auto &server    = *NimBLEDevice::createServer();
  auto &server_cb = *new ServerCallbacks();
  server.setCallbacks(&server_cb);

  auto &scan_manager = *new ScanManager();
  auto &hr_service   = *server.createService(BLE_CHAR_HR_SERVICE_UUID);
  // repeat the data from the connected device
  auto &hr_char    = *hr_service.createCharacteristic(BLE_CHAR_HR_CHAR_UUID,
                                                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  auto &white_char = *hr_service.createCharacteristic(BLE_CHAR_WHITE_LIST_UUID,
                                                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  auto &white_cb   = *new WhiteListCallback();
  white_char.setCallbacks(&white_cb);
  white_cb.on_request_address = [&scan_manager]() {
    return scan_manager.get_target_addr();
  };
  white_cb.on_disconnect = [&scan_manager]() {
    scan_manager.set_target_addr(etl::nullopt);
  };
  white_cb.on_address = [&scan_manager](WhiteListCallback::addr_opt_t addr) {
    if (addr.has_value()) {
      scan_manager.set_target_addr(etl::make_optional(addr.value().addr));
    } else {
      scan_manager.set_target_addr(etl::nullopt);
    }
  };

  /**
   * @brief should always allocate on heap when using `run_recv_task`
   */
  struct recv_task_param_t {
    std::function<void(LLCC68 &)> task;
    LLCC68 *rf;
    TaskHandle_t handle;
  };

  auto recv_task = [&scan_manager, name_map_key](LLCC68 &rf) {
    const auto TAG = "recv";
    for (;;) {
      if (RxFlag) {
        RxFlag = false;
        uint8_t data[255];
        size_t size = rf.receive(data, sizeof(data));
        if (size > 0) {
          ESP_LOGI(TAG, "recv=%s", utils::toHex(data, size).c_str());
          auto magic = data[0];
          switch (magic) {
            case HrLoRa::query_device_by_mac::magic: {
              auto r = HrLoRa::query_device_by_mac::unmarshal(data, size);
              if (!r) {
                ESP_LOGE(TAG, "failed to unmarshal query_device_by_mac");
                break;
              }
              auto &req           = r.value();
              auto my_addr        = NimBLEDevice::getAddress();
              auto my_addr_native = my_addr.getNative();
              bool is_broadcast   = std::equal(req.addr.begin(), req.addr.end(), HrLoRa::broadcast_addr.data());
              bool eq             = is_broadcast || std::equal(req.addr.begin(), req.addr.end(), my_addr_native);
              if (!eq) {
                ESP_LOGI(TAG, "%s is not for me", utils::toHex(req.addr.data(), req.addr.size()).c_str());
                break;
              }
              auto resp = HrLoRa::query_device_by_mac_response::t{
                  .repeater_addr = HrLoRa::addr_t{},
                  .key           = *name_map_key,
              };
              std::copy(my_addr_native, my_addr_native + HrLoRa::BLE_ADDR_SIZE, resp.repeater_addr.data());
              auto device = scan_manager.get_device();
              if (device) {
                auto dev = HrLoRa::hr_device::t{};
                std::copy(device->addr.begin(), device->addr.end(), dev.addr.data());
                dev.name = device->name;
              } else {
                resp.device = etl::nullopt;
              }
              uint8_t buf[64];
              auto sz = HrLoRa::query_device_by_mac_response::marshal(resp, buf, sizeof(buf));
              if (sz == 0) {
                ESP_LOGE(TAG, "failed to marshal query_device_by_mac_response");
                break;
              }
              rf.standby();
              auto err = rf.transmit(buf, sz);
              if (err != RADIOLIB_ERR_NONE) {
                ESP_LOGE(TAG, "failed to transmit, code %d", err);
              } else {
                ESP_LOGI(TAG, "tx=%s (%d)", utils::toHex(buf, sz).c_str(), sz);
              }
            }
            case HrLoRa::set_name_map_key::magic: {
              auto r = HrLoRa::set_name_map_key::unmarshal(data, size);
              if (!r) {
                ESP_LOGE("recv", "failed to unmarshal set_name_map_key");
                break;
              }
              auto &req     = r.value();
              *name_map_key = req.key;
              app_nvs::set_name_map_key(req.key);
              break;
            }
            case HrLoRa::hr_data::magic:
            case HrLoRa::query_device_by_mac_response::magic: {
              // from other repeater. do nothing.
              continue;
            }
            default: {
              ESP_LOGW("recv", "unknown magic: %d", magic);
            }
          }
        }
      } else {
        taskYIELD();
      }
    }
  };

  /**
   * a helper function to run a function on a new FreeRTOS task
   */
  auto run_recv_task = [](void *pvParameter) {
    auto param = reinterpret_cast<recv_task_param_t *>(pvParameter);
    [[unlikely]] if (param->task != nullptr && param->rf != nullptr) {
      param->task(*param->rf);
    } else {
      ESP_LOGW("recv task", "bad precondition");
    }
    auto handle = param->handle;
    delete param;
    vTaskDelete(handle);
  };
  auto recv_param = new recv_task_param_t{recv_task, &rf, nullptr};

  if (has_addr) {
    scan_manager.set_target_addr(etl::make_optional(addr));
  }

  scan_manager.on_data = [&rf, &hr_char, name_map_key](HeartMonitor &device, uint8_t *data, size_t size) {
    const auto TAG = "scan_manager";
    ESP_LOGI(TAG, "data: %s", utils::toHex(data, size).c_str());
    // https://community.home-assistant.io/t/ble-heartrate-monitor/300354/43
    // 3.103 Heart Rate Measurement of GATT Specification Supplement
    // first byte is a struct of some flags
    // if bit 0 of the first byte is 0, it's uint8
    if (size < 2) {
      ESP_LOGW(TAG, "bad data size: %d", size);
      return;
    }
    int hr;
    if ((data[0] & 0b1) == 0) {
      hr = data[1];
      ESP_LOGI(TAG, "hr=%d", hr);
    } else {
      // if bit 0 of the first byte is 1, it's uint16 and it's little endian
      hr = ::le16dec(data + 1);
      ESP_LOGI(TAG, "hr=%d (uint16le)", hr);
    }
    if (hr > 255) {
      ESP_LOGW(TAG, "hr overflow; cap to 255;");
      hr = 255;
    }
    auto hr_data = HrLoRa::hr_data::t{
        .key = *name_map_key,
        .hr  = static_cast<uint8_t>(hr),
    };
    uint8_t buf[16];
    auto sz = HrLoRa::hr_data::marshal(hr_data, buf, sizeof(buf));
    if (sz == 0) {
      ESP_LOGE(TAG, "failed to marshal hr_data");
      return;
    }
    rf.standby();
    // for LoRa we encode the data as `HrLoRa::hr_data`
    auto err = rf.transmit(buf, sz);
    if (err != RADIOLIB_ERR_NONE) {
      ESP_LOGE(TAG, "failed to transmit, code %d", err);
    }
    // for Bluetooth LE character we just repeat the data
    hr_char.setValue(data, size);
    hr_char.notify();
    rf.standby();
    rf.startReceive();
  };

  server.start();
  scan_manager.start_scanning_task();
  NimBLEDevice::startAdvertising();

  xTaskCreate(run_recv_task, "recv_task", 4096, recv_param, 1, &recv_param->handle);
  vTaskDelete(nullptr);
}
