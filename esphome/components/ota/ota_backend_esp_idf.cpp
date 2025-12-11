#ifdef USE_ESP32
#include "ota_backend_esp_idf.h"

#include "esphome/components/md5/md5.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>

namespace esphome {
namespace ota {

static const char *const TAG = "ota.esp_idf";

std::unique_ptr<ota::OTABackend> make_ota_backend() {
  auto backend = make_unique<ota::IDFOTABackend>();
#ifdef USE_OTA_PSRAM_MODE
  backend->set_psram_mode(true);
#endif
  return backend;
}

OTAResponseTypes IDFOTABackend::begin(size_t image_size) {
  // PSRAM mode: buffer entire firmware in PSRAM
  if (this->use_psram_) {
    if (!heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) {
      ESP_LOGE(TAG, "PSRAM not available");
      return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
    }

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram < image_size) {
      ESP_LOGE(TAG, "Not enough PSRAM: need %u, have %u", image_size, free_psram);
      return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
    }

    this->psram_buffer_ = (uint8_t *) heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM);
    if (!this->psram_buffer_) {
      ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM", image_size);
      return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
    }

    this->buffer_size_ = image_size;
    this->bytes_received_ = 0;
    this->md5_.init();
    ESP_LOGI(TAG, "PSRAM OTA: allocated %u bytes", image_size);
    return OTA_RESPONSE_OK;
  }

  // Standard mode: write directly to flash
  this->partition_ = esp_ota_get_next_update_partition(nullptr);
  if (this->partition_ == nullptr) {
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }

#if CONFIG_ESP_TASK_WDT_TIMEOUT_S < 15
  // The following function takes longer than the 5 seconds timeout of WDT
  esp_task_wdt_config_t wdtc;
  wdtc.idle_core_mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  wdtc.idle_core_mask |= (1 << 0);
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  wdtc.idle_core_mask |= (1 << 1);
#endif
  wdtc.timeout_ms = 15000;
  wdtc.trigger_panic = false;
  esp_task_wdt_reconfigure(&wdtc);
#endif

  esp_err_t err = esp_ota_begin(this->partition_, image_size, &this->update_handle_);

#if CONFIG_ESP_TASK_WDT_TIMEOUT_S < 15
  // Set the WDT back to the configured timeout
  wdtc.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
  esp_task_wdt_reconfigure(&wdtc);
#endif

  if (err != ESP_OK) {
    esp_ota_abort(this->update_handle_);
    this->update_handle_ = 0;
    if (err == ESP_ERR_INVALID_SIZE) {
      return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
    } else if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
  this->md5_.init();
  return OTA_RESPONSE_OK;
}

void IDFOTABackend::set_update_md5(const char *expected_md5) {
  memcpy(this->expected_bin_md5_, expected_md5, 32);
  this->md5_set_ = true;
}

OTAResponseTypes IDFOTABackend::write(uint8_t *data, size_t len) {
  // PSRAM mode: copy to buffer
  if (this->use_psram_) {
    if (this->bytes_received_ + len > this->buffer_size_) {
      ESP_LOGE(TAG, "PSRAM buffer overflow");
      return OTA_RESPONSE_ERROR_UNKNOWN;
    }
    memcpy(this->psram_buffer_ + this->bytes_received_, data, len);
    this->bytes_received_ += len;
    this->md5_.add(data, len);
    return OTA_RESPONSE_OK;
  }

  // Standard mode: write to flash
  esp_err_t err = esp_ota_write(this->update_handle_, data, len);
  this->md5_.add(data, len);
  if (err != ESP_OK) {
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      return OTA_RESPONSE_ERROR_MAGIC;
    } else if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::end() {
  // Verify MD5
  if (this->md5_set_) {
    this->md5_.calculate();
    if (!this->md5_.equals_hex(this->expected_bin_md5_)) {
      this->abort();
      return OTA_RESPONSE_ERROR_MD5_MISMATCH;
    }
  }

  // PSRAM mode: flash from buffer
  if (this->use_psram_) {
    ESP_LOGI(TAG, "PSRAM OTA: MD5 verified, flashing to partition");

    // Find target partition
    if (this->target_partition_label_[0] != '\0') {
      // Use specified partition label
      this->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, this->target_partition_label_);
      if (!this->partition_) {
        ESP_LOGE(TAG, "Target partition '%s' not found", this->target_partition_label_);
        this->abort();
        return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
      }
      ESP_LOGI(TAG, "Flashing to partition: %s", this->target_partition_label_);
    } else {
      // Default: find ota_0 partition
      this->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
      if (!this->partition_) {
        ESP_LOGE(TAG, "Main partition not found");
        this->abort();
        return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
      }
    }

    // Erase partition
    esp_err_t err = esp_partition_erase_range(this->partition_, 0, this->partition_->size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Partition erase failed: %d", err);
      this->abort();
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }

    // Flash from PSRAM
    const size_t CHUNK_SIZE = 4096;
    for (size_t offset = 0; offset < this->buffer_size_; offset += CHUNK_SIZE) {
      size_t chunk_len = std::min(CHUNK_SIZE, this->buffer_size_ - offset);
      err = esp_partition_write(this->partition_, offset, this->psram_buffer_ + offset, chunk_len);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Partition write failed at offset %u: %d", offset, err);
        this->abort();
        return OTA_RESPONSE_ERROR_WRITING_FLASH;
      }
      esp_task_wdt_reset();
    }

    // Verify flash write
    auto verify_buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);
    for (size_t offset = 0; offset < this->buffer_size_; offset += CHUNK_SIZE) {
      size_t chunk_len = std::min(CHUNK_SIZE, this->buffer_size_ - offset);
      err = esp_partition_read(this->partition_, offset, verify_buf.get(), chunk_len);
      if (err != ESP_OK || memcmp(verify_buf.get(), this->psram_buffer_ + offset, chunk_len) != 0) {
        ESP_LOGE(TAG, "Verification failed at offset %u", offset);
        this->abort();
        return OTA_RESPONSE_ERROR_WRITING_FLASH;
      }
      esp_task_wdt_reset();
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(this->partition_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set boot partition: %d", err);
      this->abort();
      return OTA_RESPONSE_ERROR_UPDATE_END;
    }

    this->abort();  // Free PSRAM
    ESP_LOGI(TAG, "PSRAM OTA complete");
    return OTA_RESPONSE_OK;
  }

  // Standard mode
  esp_err_t err = esp_ota_end(this->update_handle_);
  this->update_handle_ = 0;
  if (err == ESP_OK) {
    err = esp_ota_set_boot_partition(this->partition_);
    if (err == ESP_OK) {
      return OTA_RESPONSE_OK;
    }
  }
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  return OTA_RESPONSE_ERROR_UNKNOWN;
}

void IDFOTABackend::abort() {
  if (this->psram_buffer_) {
    heap_caps_free(this->psram_buffer_);
    this->psram_buffer_ = nullptr;
  }
  if (this->update_handle_) {
    esp_ota_abort(this->update_handle_);
    this->update_handle_ = 0;
  }
  this->buffer_size_ = 0;
  this->bytes_received_ = 0;
}

}  // namespace ota
}  // namespace esphome
#endif  // USE_ESP32
