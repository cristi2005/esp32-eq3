#include "sdcard.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace esphome::sdcard {

static const char *const TAG = "sdcard";

// Cat citim din card dintr-o data. 4 KB e un compromis: mai mic inseamna prea multe accese la
// card, mai mare ar manca din memoria interna care oricum e pe terminate.
static constexpr size_t BUFFER_SIZE = 4096;
static constexpr size_t MAX_TRACKS = 200;

namespace {

/// Tipul fisierului, dupa terminatie. Sirurile sunt EXACT cele pe care le recunoaste ESPHome
/// cand citeste antetul Content-Type - daca le schimbi, placa nu mai stie ce sa decodeze.
const char *mime_pentru(const std::string &nume) {
  auto punct = nume.rfind('.');
  if (punct == std::string::npos) {
    return nullptr;
  }
  std::string ext = nume.substr(punct + 1);
  for (char &c : ext) {
    c = (char) tolower((unsigned char) c);
  }
  if (ext == "mp3") {
    return "audio/mpeg";
  }
  if (ext == "wav") {
    return "audio/wav";
  }
  if (ext == "flac") {
    return "audio/flac";
  }
  if (ext == "opus" || ext == "ogg") {
    return "audio/ogg;codecs=opus";
  }
  return nullptr;
}

/// Numele fisierelor pot contine spatii si diacritice, care nu au ce cauta intr-o adresa web.
/// Le inlocuim cu %XX, si le desfacem inapoi in server.
std::string codifica_adresa(const std::string &text) {
  static const char *const HEXA = "0123456789ABCDEF";
  std::string iesire;
  iesire.reserve(text.size() + 8);
  for (unsigned char c : text) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      iesire.push_back((char) c);
    } else {
      iesire.push_back('%');
      iesire.push_back(HEXA[c >> 4]);
      iesire.push_back(HEXA[c & 0x0F]);
    }
  }
  return iesire;
}

std::string decodifica_adresa(const char *text) {
  std::string iesire;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
      char hexa[3] = {p[1], p[2], '\0'};
      iesire.push_back((char) strtol(hexa, nullptr, 16));
      p += 2;
    } else {
      iesire.push_back(*p);
    }
  }
  return iesire;
}

}  // namespace

void SDCard::setup() {
  // Modul pe 1 fir foloseste doar GPIO 14, 15 si 2. Pe 4 fire ar mai cere 4, 12 si 13, iar
  // GPIO13 e butonul KEY2. E de vreo trei ori mai lent, dar un fisier de muzica cere sub
  // 40 KB pe secunda, deci nu se simte.
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

  this->scan_music();
  this->start_http_server_();
}

void SDCard::start_http_server_() {
  if (this->http_port_ == 0) {
    return;
  }

  // Rezerva de citire o cerem intai din memoria externa (PSRAM), ca sa nu consumam din cea
  // interna, care e la 74%. Daca nu e PSRAM, se ia din cea interna.
  RAMAllocator<uint8_t> allocator;
  this->buffer_ = allocator.allocate(BUFFER_SIZE);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Nu am memorie pentru rezerva de citire - serverul nu porneste.");
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->http_port_;
  // ATENTIE: pagina web a placii foloseste deja portul de control implicit (32768). Doua
  // servere cu acelasi port de control nu pornesc - de-aia il mutam pe al nostru.
  config.ctrl_port = 32769;
  config.max_open_sockets = 2;
  config.stack_size = 4096;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&this->server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Serverul de fisiere nu a pornit: %s", esp_err_to_name(err));
    this->server_ = nullptr;
    return;
  }

  httpd_uri_t uri = {};
  uri.uri = "/*";
  uri.method = HTTP_GET;
  uri.handler = SDCard::http_handler_;
  uri.user_ctx = this;
  httpd_register_uri_handler(this->server_, &uri);

  ESP_LOGI(TAG, "Server de fisiere pornit pe portul %u", this->http_port_);
}

esp_err_t SDCard::http_handler_(httpd_req_t *request) {
  auto *self = (SDCard *) request->user_ctx;

  // Sarim peste "/" de la inceput si taiem eventualul "?..." de la sfarsit.
  std::string nume = decodifica_adresa(request->uri + 1);
  auto intrebare = nume.find('?');
  if (intrebare != std::string::npos) {
    nume.resize(intrebare);
  }

  // Fara ".." si fara cai absolute: altfel oricine din retea ar putea cere orice fisier.
  if (nume.empty() || nume.find("..") != std::string::npos || nume[0] == '/') {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Nume de fisier nepermis");
    return ESP_FAIL;
  }

  const char *mime = mime_pentru(nume);
  if (mime == nullptr) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Nu e fisier de muzica");
    return ESP_FAIL;
  }

  std::string cale = self->music_folder_ + "/" + nume;
  FILE *fisier = fopen(cale.c_str(), "rb");
  if (fisier == nullptr) {
    ESP_LOGW(TAG, "Nu gasesc fisierul %s", cale.c_str());
    httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Fisier inexistent");
    return ESP_FAIL;
  }

  // Tipul trebuie trimis corect: placa se uita la el ca sa stie ce decodor sa foloseasca.
  httpd_resp_set_type(request, mime);

  size_t citit;
  while ((citit = fread(self->buffer_, 1, BUFFER_SIZE, fisier)) > 0) {
    if (httpd_resp_send_chunk(request, (const char *) self->buffer_, citit) != ESP_OK) {
      // Playerul a inchis legatura (ai schimbat melodia, de exemplu) - nu e o eroare.
      fclose(fisier);
      return ESP_FAIL;
    }
  }
  fclose(fisier);

  // Bucata de lungime zero inseamna "am terminat".
  httpd_resp_send_chunk(request, nullptr, 0);
  return ESP_OK;
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
    if (mime_pentru(intrare->d_name) == nullptr) {
      continue;  // nu e fisier de muzica
    }
    this->tracks_.emplace_back(intrare->d_name);
    if (this->tracks_.size() >= MAX_TRACKS) {
      ESP_LOGW(TAG, "M-am oprit la %d melodii.", (int) MAX_TRACKS);
      break;
    }
  }
  closedir(dir);

  std::sort(this->tracks_.begin(), this->tracks_.end());

  ESP_LOGI(TAG, "Am gasit %d melodii in %s:", (int) this->tracks_.size(), this->music_folder_.c_str());
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

std::string SDCard::track_url(int index) const {
  if (index < 0 || index >= (int) this->tracks_.size()) {
    return {};
  }
  // 127.0.0.1 inseamna "placa insasi" - cererea nu iese in retea, deci merge si fara WiFi.
  return "http://127.0.0.1:" + std::to_string(this->http_port_) + "/" + codifica_adresa(this->tracks_[index]);
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
  ESP_LOGCONFIG(TAG, "  Server fisiere: %s (port %u)", this->server_ != nullptr ? "pornit" : "oprit", this->http_port_);
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
