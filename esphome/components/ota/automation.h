#pragma once

#include "esphome/core/automation.h"

#ifdef USE_ESP32
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

namespace esphome {
namespace ota {

#ifdef USE_OTA_STATE_CALLBACK
#include "ota_backend.h"

class OTAStateChangeTrigger : public Trigger<OTAState> {
 public:
  explicit OTAStateChangeTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (!parent->is_failed()) {
        trigger(state);
      }
    });
  }
};

class OTAStartTrigger : public Trigger<> {
 public:
  explicit OTAStartTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (state == OTA_STARTED && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class OTAProgressTrigger : public Trigger<float> {
 public:
  explicit OTAProgressTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (state == OTA_IN_PROGRESS && !parent->is_failed()) {
        trigger(progress);
      }
    });
  }
};

class OTAEndTrigger : public Trigger<> {
 public:
  explicit OTAEndTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (state == OTA_COMPLETED && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class OTAAbortTrigger : public Trigger<> {
 public:
  explicit OTAAbortTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (state == OTA_ABORT && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class OTAErrorTrigger : public Trigger<uint8_t> {
 public:
  explicit OTAErrorTrigger(OTAComponent *parent) {
    parent->add_on_state_callback([this, parent](OTAState state, float progress, uint8_t error) {
      if (state == OTA_ERROR && !parent->is_failed()) {
        trigger(error);
      }
    });
  }
};

#endif  // USE_OTA_STATE_CALLBACK

template<typename... Ts> class SwitchPartitionAndRebootAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, partition_label)

  void play(Ts... x) override {
#ifdef USE_ESP32
    auto label = this->partition_label_.value(x...);
    if (label.empty()) {
      ESP_LOGE("ota", "Partition label is empty");
      return;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());

    if (!partition) {
      ESP_LOGE("ota", "Partition '%s' not found", label.c_str());
      return;
    }

    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
      ESP_LOGE("ota", "Failed to set boot partition '%s': %d", label.c_str(), err);
      return;
    }

    ESP_LOGI("ota", "Rebooting to partition: %s", label.c_str());
    delay(100);  // Give log time to flush
    esp_restart();
#else
    ESP_LOGE("ota", "Partition switching only supported on ESP32");
#endif
  }
};

}  // namespace ota
}  // namespace esphome
