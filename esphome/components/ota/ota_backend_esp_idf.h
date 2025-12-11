#pragma once
#ifdef USE_ESP32
#include "ota_backend.h"

#include "esphome/components/md5/md5.h"
#include "esphome/core/defines.h"

#include <esp_ota_ops.h>

namespace esphome {
namespace ota {

class IDFOTABackend : public OTABackend {
 public:
  OTAResponseTypes begin(size_t image_size) override;
  void set_update_md5(const char *md5) override;
  OTAResponseTypes write(uint8_t *data, size_t len) override;
  OTAResponseTypes end() override;
  void abort() override;
  bool supports_compression() override { return false; }

  void set_psram_mode(bool enable) { this->use_psram_ = enable; }
  void set_target_partition(const char *label) {
    if (label && strlen(label) > 0) {
      strncpy(this->target_partition_label_, label, sizeof(this->target_partition_label_) - 1);
      this->target_partition_label_[sizeof(this->target_partition_label_) - 1] = '\0';
    }
  }

 protected:
  esp_ota_handle_t update_handle_{0};
  const esp_partition_t *partition_{nullptr};
  md5::MD5Digest md5_{};
  char expected_bin_md5_[32];
  bool md5_set_{false};
  bool use_psram_{false};

  // PSRAM buffering (ESP32-P4 dual-partition mode)
  uint8_t *psram_buffer_{nullptr};
  size_t buffer_size_{0};
  size_t bytes_received_{0};
  char target_partition_label_[17]{};  // ESP partition label max 16 chars + null
};

}  // namespace ota
}  // namespace esphome
#endif  // USE_ESP32
