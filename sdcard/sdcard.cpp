#include "sdcard.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace esphome::sdcard {

static const char *const TAG = "sdcard";

// Bufferul de citire prin memoria interna. Redus la 8 KB (de la 16 KB): in teste reale, cu
// radio/Bluetooth active, alocarea de 16 KB in memoria INTERNA (DMA) a esuat cel putin o data -
// memoria interna e mult mai putina si mai disputata decat cea externa (PSRAM), si 8 KB tot e
// suficient de mare cat sa nu incetineasca simtitor citirea.
static constexpr size_t CITIRE_BUFFER = 8 * 1024;
static constexpr size_t MAX_TRACKS = 200;
// Cat de mare poate fi o melodie ca sa incapa in memoria externa. La 128 kbps, 12 MB inseamna
// peste 12 minute de muzica - ar trebui sa acopere orice fisier normal. Daca nu incape (sau nu
// mai e loc din cauza altor lucruri care folosesc PSRAM), load_track() refuza politicos, cu
// avertisment in log, fara sa opreasca placa.
static constexpr size_t MELODIE_MAXIM = 12 * 1024 * 1024;

namespace {

/// Tipul fisierului, dupa terminatie. Fiecare varianta e aparata cu #ifdef, pentru ca placa nu
/// are neaparat compilate toate cele patru decodoare (aici, doar MP3 e sigur prezent).
audio::AudioFileType tip_fisier_pentru(const std::string &nume) {
  auto punct = nume.rfind('.');
  if (punct == std::string::npos) {
    return audio::AudioFileType::NONE;
  }
  std::string ext = nume.substr(punct + 1);
  for (char &c : ext) {
    c = (char) tolower((unsigned char) c);
  }
#ifdef USE_AUDIO_MP3_SUPPORT
  if (ext == "mp3") {
    return audio::AudioFileType::MP3;
  }
#endif
#ifdef USE_AUDIO_WAV_SUPPORT
  if (ext == "wav") {
    return audio::AudioFileType::WAV;
  }
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
  if (ext == "flac") {
    return audio::AudioFileType::FLAC;
  }
#endif
#ifdef USE_AUDIO_OPUS_SUPPORT
  if (ext == "opus") {
    return audio::AudioFileType::OPUS;
  }
#endif
  return audio::AudioFileType::NONE;
}

}  // namespace

void SDCard::setup() {
  // Modul pe 1 fir foloseste doar GPIO 14, 15 si 2. Pe 4 fire ar mai cere 4, 12 si 13, iar
  // GPIO13 e butonul KEY2. E de vreo trei ori mai lent, dar noi citim o melodie intreaga o
  // singura data la schimbarea ei, nu in timp real, deci nu conteaza.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
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
      ESP_LOGE(TAG,
               "Cardul nu a putut fi initializat: %s. Verifica daca e bagat pana la capat "
               "si daca e formatat FAT32.",
               esp_err_to_name(err));
    }
    this->mark_failed();
    return;
  }

  this->mounted_ = true;
  this->size_mb_ = ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL);
  std::strncpy(this->name_, card->cid.name, sizeof(this->name_) - 1);
  ESP_LOGI(TAG, "Card montat la %s: %s, %llu MB", this->mount_point_.c_str(), this->name_, this->size_mb_);

  // Cat PSRAM avem disponibil chiar de la bootare, INAINTE sa fi incarcat vreo melodie - ne
  // trebuie ca reper, ca sa stim daca esecurile de alocare de mai tarziu sunt din lipsa reala de
  // memorie sau din fragmentare (bloc mare liber mult mai mic decat totalul liber).
  ESP_LOGI(TAG, "PSRAM la boot: %u KB liberi total, %u KB in cel mai mare bloc continuu.",
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

  this->scan_music();
}

bool SDCard::track_fits(int index) const {
  if (index < 0 || index >= (int) this->tracks_.size()) {
    return false;
  }
  const std::string cale = this->music_folder_ + "/" + this->tracks_[index];
  struct stat info;
  if (stat(cale.c_str(), &info) != 0 || info.st_size <= 0) {
    return false;
  }
  const size_t marime = (size_t) info.st_size;
  // Dupa ce bufferul curent se elibereaza (chiar inainte de alocarea noua, in load_track()),
  // acel spatiu devine din nou disponibil - il adaugam la calcul, altfel am refuza pe nedrept
  // orice melodie de aceeasi marime cu cea care tocmai canta.
  const size_t curent = (this->buffer_curent_ != nullptr) ? this->fisier_curent_.length : 0;
  const size_t liber_dupa_eliberare = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) + curent;
  // Marja de siguranta: alocatorul de PSRAM mai pierde cate ceva la fiecare alocare/eliberare
  // (antet, aliniere) - nu ne bazam pe granita exacta.
  const size_t marja_siguranta = 64 * 1024;
  return marime + marja_siguranta <= liber_dupa_eliberare;
}

audio::AudioFile *SDCard::load_track(int index) {
  if (index < 0 || index >= (int) this->tracks_.size()) {
    ESP_LOGW(TAG, "Index de melodie invalid: %d", index);
    return nullptr;
  }

  const std::string &nume = this->tracks_[index];
  const audio::AudioFileType tip = tip_fisier_pentru(nume);
  if (tip == audio::AudioFileType::NONE) {
    ESP_LOGW(TAG, "Tip de fisier necunoscut sau nesuportat: %s", nume.c_str());
    return nullptr;
  }

  const std::string cale = this->music_folder_ + "/" + nume;
  FILE *fisier = fopen(cale.c_str(), "rb");
  if (fisier == nullptr) {
    ESP_LOGW(TAG, "Nu gasesc fisierul %s", cale.c_str());
    return nullptr;
  }

  if (fseek(fisier, 0, SEEK_END) != 0) {
    fclose(fisier);
    ESP_LOGW(TAG, "Nu pot afla marimea fisierului %s", nume.c_str());
    return nullptr;
  }
  const long marime = ftell(fisier);
  fseek(fisier, 0, SEEK_SET);
  if (marime <= 0) {
    fclose(fisier);
    ESP_LOGW(TAG, "Fisier gol sau ilizibil: %s", nume.c_str());
    return nullptr;
  }
  if ((size_t) marime > MELODIE_MAXIM) {
    fclose(fisier);
    ESP_LOGW(TAG, "Melodia %s e prea mare (%ld KB, maxim %u KB) - o sar.", nume.c_str(), marime / 1024,
             (unsigned) (MELODIE_MAXIM / 1024));
    return nullptr;
  }

  // Eliberam bufferul melodiei ANTERIOARE ACUM, inainte sa alocam cel nou - e sigur doar pentru
  // ca apelantul (schimba_melodie din YAML) a oprit deja redarea si a asteptat o pauza inainte
  // sa cheme load_track() din nou (acelasi tipar stop+pauza folosit si la trecerea pe Bluetooth).
  // Asa fiecare melodie foloseste tot bugetul de PSRAM disponibil, nu doar jumatate din el, cat
  // am tine si melodia veche "de rezerva" pana la urmatoarea schimbare.
  if (this->buffer_curent_ != nullptr) {
    heap_caps_free(this->buffer_curent_);
    this->buffer_curent_ = nullptr;
    this->fisier_curent_ = {};
  }

  uint8_t *bufer_nou = (uint8_t *) heap_caps_malloc((size_t) marime, MALLOC_CAP_SPIRAM);
  if (bufer_nou == nullptr) {
    fclose(fisier);
    ESP_LOGE(TAG,
             "Nu (mai) am memorie externa pentru %s (%ld KB). PSRAM liber: %u KB total, %u KB cel "
             "mai mare bloc continuu. Melodia anterioara a fost oprita si golita din memorie.",
             nume.c_str(), marime / 1024, (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    return nullptr;
  }

  // AICI ERA CAUZA INTRERUPERILOR, si e scrisa in documentatia Espressif despre driverul de
  // card: canalul de transfer direct al cititorului de card nu ajunge in memoria EXTERNA. Daca
  // am citi direct in "bufer_nou" (care e in PSRAM) cu fread(), driverul ar copia bloc cu bloc,
  // cate 512 octeti, fiecare cu comanda si alocare proprie - mult mai incet. De-aia citim intai
  // prin "citire_", care e in memoria INTERNA (rapid), si abia apoi copiem bucata cu memcpy() in
  // bufferul mare din memoria externa - o simpla copiere in memorie, fara nicio implicare a
  // cardului, deci instantanee.
  if (this->citire_ == nullptr) {
    this->citire_ = (uint8_t *) heap_caps_malloc(CITIRE_BUFFER, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (this->citire_ == nullptr) {
      ESP_LOGW(TAG, "Nu am memorie interna pentru bufferul de citire - citesc mai incet, direct.");
    }
  }

  const int64_t start_us = esp_timer_get_time();
  size_t scris = 0;
  while (scris < (size_t) marime) {
    const size_t de_citit = std::min(CITIRE_BUFFER, (size_t) marime - scris);
    size_t citit;
    if (this->citire_ != nullptr) {
      citit = fread(this->citire_, 1, de_citit, fisier);
      if (citit > 0) {
        std::memcpy(bufer_nou + scris, this->citire_, citit);
      }
    } else {
      // Plasa de siguranta, daca nu s-a putut aloca bufferul mic: citim direct in memoria
      // externa - mai incet, dar macar functioneaza.
      citit = fread(bufer_nou + scris, 1, de_citit, fisier);
    }
    if (citit == 0) {
      break;
    }
    scris += citit;
  }
  fclose(fisier);

  if (scris != (size_t) marime) {
    ESP_LOGW(TAG, "Citire incompleta pentru %s: %u din %ld octeti - o sar.", nume.c_str(), (unsigned) scris,
             marime);
    heap_caps_free(bufer_nou);
    return nullptr;
  }

  const int64_t durata_ms = (esp_timer_get_time() - start_us) / 1000;
  ESP_LOGI(TAG,
           "Melodia \"%s\" incarcata in memoria externa: %u KB in %lld ms (%.1f KB/s). PSRAM liber "
           "dupa: %u KB total, %u KB cel mai mare bloc continuu.",
           nume.c_str(), (unsigned) (marime / 1024), (long long) durata_ms,
           durata_ms > 0 ? (marime / 1024.0) / (durata_ms / 1000.0) : 0.0,
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

  this->buffer_curent_ = bufer_nou;
  this->fisier_curent_.data = this->buffer_curent_;
  this->fisier_curent_.length = (size_t) marime;
  this->fisier_curent_.file_type = tip;
  return &this->fisier_curent_;
}

void SDCard::scan_music() {
  this->tracks_.clear();
  if (!this->mounted_) {
    ESP_LOGW(TAG, "Cardul nu e montat - nu am ce cauta.");
    return;
  }

  DIR *dir = opendir(this->music_folder_.c_str());
  if (dir == nullptr) {
    ESP_LOGW(TAG, "Nu pot deschide folderul de muzica %s", this->music_folder_.c_str());
    return;
  }

  struct dirent *intrare;
  while ((intrare = readdir(dir)) != nullptr) {
    if (intrare->d_type == DT_DIR) {
      continue;
    }
    if (tip_fisier_pentru(intrare->d_name) == audio::AudioFileType::NONE) {
      continue;  // nu e fisier de muzica recunoscut
    }
    this->tracks_.emplace_back(intrare->d_name);
    if (this->tracks_.size() >= MAX_TRACKS) {
      ESP_LOGW(TAG, "M-am oprit la %d melodii.", (int) MAX_TRACKS);
      break;
    }
  }
  closedir(dir);

  std::sort(this->tracks_.begin(), this->tracks_.end());

  ESP_LOGI(TAG, "Am gasit %d melodii in %s.", (int) this->tracks_.size(), this->music_folder_.c_str());
}

void SDCard::log_tracks() {
  ESP_LOGI(TAG, "Lista de melodii (%d):", (int) this->tracks_.size());
  for (size_t i = 0; i < this->tracks_.size(); i++) {
    ESP_LOGI(TAG, "  %2d. %s", (int) (i + 1), this->tracks_[i].c_str());
  }
}

std::string SDCard::track_name(int index) const {
  if (index < 0 || index >= (int) this->tracks_.size()) {
    return {};
  }
  return this->tracks_[index];
}

void SDCard::dump_config() {
  ESP_LOGCONFIG(TAG, "Card SD:");
  ESP_LOGCONFIG(TAG, "  Folder: %s", this->mount_point_.c_str());
  ESP_LOGCONFIG(TAG, "  Muzica: %s", this->music_folder_.c_str());
  if (this->mounted_) {
    ESP_LOGCONFIG(TAG, "  Montat: da (%s, %llu MB, %d melodii)", this->name_, this->size_mb_,
                  (int) this->tracks_.size());
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
