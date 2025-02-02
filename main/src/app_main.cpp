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
#include <freertos/event_groups.h>
#include <cstring>

extern "C" void app_main();

const auto RecvEvt = BIT0;

/**
 * @brief try to transmit the data
 * @note would block until the transmission is done and will start receiving after that
 */
void tryTransmit(uint8_t *data, size_t size, LLCC68 &rf) {
  const auto TAG = "tryTransmit";
  auto err       = rf.transmit(data, size);
  if (err == RADIOLIB_ERR_NONE) {
    // ok
  } else if (err == RADIOLIB_ERR_TX_TIMEOUT) {
    ESP_LOGW(TAG, "tx timeout; please check the busy pin;");
  } else {
    ESP_LOGE(TAG, "failed to transmit, code %d", err);
  }
  rf.standby();
  rf.startReceive();
}

struct handle_message_callbacks_t {
  std::function<void(uint8_t *data, size_t size)> send          = nullptr;
  std::function<etl::optional<blue::HeartMonitor>()> get_device = nullptr;
  std::function<void(HrLoRa::name_map_key_t)> set_name_map_key  = nullptr;
  std::function<HrLoRa::name_map_key_t()> get_name_map_key      = nullptr;
};

/**
 * @brief handle the message received from LoRa
 * @param data the data received
 * @param size the size of the data
 * @param callbacks the callbacks to handle the message. This function would do nothing if any of the callback is empty.
 */
void handle_message(uint8_t *data, size_t size, const handle_message_callbacks_t &callbacks) {
  const auto TAG   = "recv";
  bool is_cb_empty = callbacks.send == nullptr ||
                     callbacks.get_device == nullptr ||
                     callbacks.set_name_map_key == nullptr ||
                     callbacks.get_name_map_key == nullptr;
  if (is_cb_empty) {
    ESP_LOGE(TAG, "at least one callback is empty");
    return;
  }
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
          .key           = callbacks.get_name_map_key(),
      };
      std::copy(my_addr_native, my_addr_native + HrLoRa::BLE_ADDR_SIZE, resp.repeater_addr.data());
      auto device = callbacks.get_device();
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
      callbacks.send(buf, sz);
      break;
    }
    case HrLoRa::set_name_map_key::magic: {
      auto r = HrLoRa::set_name_map_key::unmarshal(data, size);
      if (!r) {
        ESP_LOGE(TAG, "failed to unmarshal set_name_map_key");
        break;
      }
      auto &req = r.value();
      callbacks.set_name_map_key(req.key);
      app_nvs::set_name_map_key(req.key);
      ESP_LOGI(TAG, "set name map key to %d", req.key);
      break;
    }
    case HrLoRa::hr_data::magic:
    case HrLoRa::query_device_by_mac_response::magic: {
      // from other repeater. do nothing.
      return;
    }
    default: {
      ESP_LOGW("recv", "unknown magic: %d", magic);
    }
  }
}

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
  static auto name_map_key = HrLoRa::name_map_key_t{0};
  auto name_map_key_ptr    = &name_map_key;
  err                      = app_nvs::get_name_map_key(name_map_key_ptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "no name map key, fallback back to 0; reason %s (%d);", esp_err_to_name(err), err);
  } else {
    ESP_LOGI(TAG, "name map key=%d", *name_map_key_ptr);
  }

  static auto hal = ESPHal(pin::SCK, pin::MISO, pin::MOSI);
  hal.init();
  ESP_LOGI(TAG, "hal init success!");
  static auto module = Module(&hal, pin::CS, pin::DIO1, pin::RST, pin::BUSY);
  static auto rf     = LLCC68(&module);
  auto st            = rf.begin(434, 500.0, 7, 7,
                                RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                                22, 8, 1.6);
  if (st != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "failed, code %d", st);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
  }
  ESP_LOGI(TAG, "RF began!");

  /** Radio receive interruption */
  struct rf_recv_interrupt_data_t {
    EventGroupHandle_t evt_grp;
  };
  auto evt_grp = xEventGroupCreate();

  /**
   * used in `rf.setPacketReceivedAction`
   * a workaround to pass an argument to ISR.
   * Actually ESP IDF has a API to pass an argument to ISR (`gpio_isr_handler_add`),
   * but RadioLib doesn't use it and I don't want to break the API (though I can).
   */
  static auto rf_recv_interrupt_data = rf_recv_interrupt_data_t{evt_grp};
  rf.setPacketReceivedAction([]() {
    // https://www.freertos.org/xEventGroupSetBitsFromISR.html
    BaseType_t task_woken = pdFALSE;
    auto xResult          = xEventGroupSetBitsFromISR(rf_recv_interrupt_data.evt_grp, RecvEvt, &task_woken);
    if (xResult != pdFAIL) {
      portYIELD_FROM_ISR(task_woken);
    }
  });

  rf.standby();
  rf.startReceive();

  NimBLEDevice::init(BLE_NAME);
  auto &server          = *NimBLEDevice::createServer();
  static auto server_cb = ServerCallbacks();
  server.setCallbacks(&server_cb);

  static auto scan_manager = ScanManager();
  auto &hr_service         = *server.createService(BLE_CHAR_HR_SERVICE_UUID);
  // repeat the data from the connected device
  auto &hr_char        = *hr_service.createCharacteristic(BLE_CHAR_HR_CHAR_UUID,
                                                          NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  auto &white_char     = *hr_service.createCharacteristic(BLE_CHAR_WHITE_LIST_UUID,
                                                          NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  auto &device_char    = *hr_service.createCharacteristic(BLE_CHAR_DEVICE_UUID,
                                                          NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  static auto white_cb = WhiteListCallback();
  white_char.setCallbacks(&white_cb);
  white_cb.on_request_address = []() {
    return scan_manager.get_target_addr();
  };
  white_cb.on_disconnect = []() {
    scan_manager.set_target_addr(etl::nullopt);
  };
  white_cb.on_address = [](WhiteListCallback::addr_opt_t addr) {
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
    EventGroupHandle_t evt_grp;
  };

  static auto handle_message_callbacks = handle_message_callbacks_t{
      .send             = [](uint8_t *data, size_t size) { tryTransmit(data, size, rf); },
      .get_device       = []() { return scan_manager.get_device(); },
      .set_name_map_key = [name_map_key_ptr](HrLoRa::name_map_key_t key) { *name_map_key_ptr = key; },
      .get_name_map_key = [name_map_key_ptr]() { return *name_map_key_ptr; },
  };

  auto recv_task = [evt_grp](LLCC68 &rf) {
    const auto TAG = "recv";
    for (;;) {
      xEventGroupWaitBits(evt_grp, RecvEvt, pdTRUE, pdFALSE, portMAX_DELAY);
      uint8_t data[255];
      size_t size = rf.receive(data, sizeof(data));
      if (size == 0) {
        ESP_LOGW(TAG, "empty data");
      }
      ESP_LOGI(TAG, "recv=%s", utils::toHex(data, size).c_str());
      handle_message(data, size, handle_message_callbacks);
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
  static auto recv_param = recv_task_param_t{recv_task, &rf, nullptr, evt_grp};

  scan_manager.on_result = [&device_char](std::string device_name, const uint8_t *addr) {
    uint8_t buf[32]                 = {0};
    auto TAG                        = "on_result";
    auto ostream                    = pb_ostream_from_buffer(buf, sizeof(buf));
    ::bluetooth_device_pb device_pb = bluetooth_device_pb_init_zero;
    device_pb.mac.funcs.encode      = [](pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
      const auto addr_ptr = reinterpret_cast<const uint8_t *>(*arg);
      if (!pb_encode_tag_for_field(stream, field)) {
        return false;
      }
      return pb_encode_string(stream, addr_ptr, white_list::BLE_MAC_ADDR_SIZE);
    };
    device_pb.mac.arg           = const_cast<uint8_t *>(addr);
    const auto target_name_size = sizeof(device_pb.name) - 1;
    if (device_name.size() > target_name_size) {
      device_name.resize(target_name_size);
      ESP_LOGW(TAG, "Truncated device name to %s", device_name.c_str());
    }
    std::memcpy(device_pb.name, device_name.c_str(), device_name.size());
    auto ok = pb_encode(&ostream, bluetooth_device_pb_fields, &device_pb);
    if (!ok) {
      ESP_LOGE(TAG, "Failed to encode the device");
      return;
    }
    auto sz = ostream.bytes_written;
    device_char.setValue(buf, sz);
    device_char.notify();
  };

  scan_manager.on_data = [&hr_char, name_map_key_ptr](HeartMonitor &device, uint8_t *data, size_t size) {
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
        .key = *name_map_key_ptr,
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
    tryTransmit(buf, sz, rf);
    // for Bluetooth LE character we just repeat the data
    hr_char.setValue(data, size);
    hr_char.notify();
  };

  /**
   * the server should be started before scanning and advertising
   */
  hr_service.start();
  server.start();
  NimBLEDevice::startAdvertising();

  if (has_addr) {
    scan_manager.set_target_addr(etl::make_optional(addr));
  }

  scan_manager.start_scanning_task();
  xTaskCreate(run_recv_task, "recv_task", 4096, &recv_param, 0, &recv_param.handle);
  vTaskDelete(nullptr);
}
