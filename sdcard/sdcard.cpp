#include "sdcard.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace esphome::sdcard {

static const char *const TAG = "sdcard";

void SDCard::setup() {
  // Modul pe 1 fir foloseste doar GPIO 14, 15 si 2. Pe 4 fire ar mai avea nevoie de 4, 12 si 13,
  // iar GPIO13 e butonul KEY2 - de-aia ramanem pe 1 fir. E de vreo trei ori mai lent, dar pentru
  // un fisier de muzica (nici 40 KB pe secunda) e mai mult decat suficient.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  // Pinii cardului au nevoie de rezistente catre plus. Placa are unele pe ea, dar le pornim si
  // pe cele din procesor: daca ale placii lipsesc, cardul tot e vazut.
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  // NU formatam niciodata singuri cardul daca montarea esueaza - ar sterge tot ce e pe el.
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 3;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_card_t *card = nullptr;
  esp_err_t err = esp_vfs_fat_sdmmc_mount(this->mount_point_.c_str(), &host, &slot_config, &mount_config, &card);
  if (err != ESP_OK) {
    if (err == ESP_FAIL) {
      ESP_LOGE(TAG, "Cardul a raspuns, dar nu are un sistem de fisiere FAT. Formateaza-l FAT32.");
    } else {
      ESP_LOGE(TAG, "Cardul nu a putut fi initializat: %s. Verifica daca e bagat pana la capat "
                    "si daca e format FAT32.",
               esp_err_to_name(err));
    }
    this->mark_failed();
    return;
  }

  this->mounted_ = true;
  this->size_mb_ = ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL);
  std::strncpy(this->name_, card->cid.name, sizeof(this->name_) - 1);

  ESP_LOGI(TAG, "Card montat la %s: %s, %llu MB", this->mount_point_.c_str(), this->name_, this->size_mb_);
  this->list_files(this->mount_point_);
}

void SDCard::dump_config() {
  ESP_LOGCONFIG(TAG, "Card SD:");
  ESP_LOGCONFIG(TAG, "  Folder: %s", this->mount_point_.c_str());
  if (this->mounted_) {
    ESP_LOGCONFIG(TAG, "  Montat: da (%s, %llu MB)", this->name_, this->size_mb_);
  } else {
    ESP_LOGCONFIG(TAG, "  Montat: NU");
  }
}

void SDCard::list_files(const std::string &path) {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "Cardul nu e montat - nu am ce lista.");
    return;
  }

  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    ESP_LOGW(TAG, "Nu pot deschide folderul %s", path.c_str());
    return;
  }

  ESP_LOGI(TAG, "Continutul folderului %s:", path.c_str());
  int numar = 0;
  struct dirent *intrare;
  while ((intrare = readdir(dir)) != nullptr) {
    if (intrare->d_type == DT_DIR) {
      ESP_LOGI(TAG, "  [folder] %s", intrare->d_name);
    } else {
      std::string cale = path + "/" + intrare->d_name;
      struct stat info;
      if (stat(cale.c_str(), &info) == 0) {
        ESP_LOGI(TAG, "  %s  (%lu KB)", intrare->d_name, (unsigned long) (info.st_size / 1024));
      } else {
        ESP_LOGI(TAG, "  %s", intrare->d_name);
      }
    }
    if (++numar >= 100) {
      ESP_LOGI(TAG, "  ... m-am oprit la 100 de intrari.");
      break;
    }
  }
  closedir(dir);
  ESP_LOGI(TAG, "Am gasit %d intrari in %s", numar, path.c_str());
}

}  // namespace esphome::sdcard

#endif

