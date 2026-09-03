#include "sdcard.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "driver/sdmmc_host.h"
#include "esp_timer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace esphome::sdcard {

static const char *const TAG = "sdcard";

// Cat citim din card dintr-o data. Marit de la 4 KB la 16 KB dupa ce s-au auzit intreruperi:
// citirile mari inseamna mai putine drumuri la card si mai putine bucati trimise, deci mai
// putine ocazii ca rezerva de sunet sa se goleasca intre doua citiri. Nu ne costa memorie
// interna - rezerva se cere din PSRAM.
static constexpr size_t BUFFER_SIZE = 16384;
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

/// Calitatea fisierului in kbps.
///
/// ATENTIE, aici gresisem prima data: la inceputul redarii serverul NU e franat de player, ci
/// toarna cat poate de repede ca sa umple rezervele. Socotind de la secunda zero ieseau cifre
/// imposibile - 844 kbps pentru un MP3, ceea ce nu exista. Abia dupa ce rezervele s-au umplut
/// livrarea se aseaza la ritmul real al muzicii.
/// De-aia masuram doar din momentul in care lucrurile s-au linistit, si returnam 0 cat timp nu
/// avem inca o bucata de timp asezata destul de lunga ca sa insemne ceva.
unsigned kbps(size_t octeti_acum, size_t octeti_la_start, int64_t moment_start_us) {
  if (moment_start_us == 0) {
    return 0;  // n-am apucat sa intram in regim asezat
  }
  const int64_t durata_us = esp_timer_get_time() - moment_start_us;
  if (durata_us < 10000000) {
    return 0;  // sub 10 secunde de regim asezat, socoteala inca minte
  }
  const uint64_t octeti = (uint64_t) (octeti_acum - octeti_la_start);
  return (unsigned) ((octeti * 8ULL * 1000000ULL) / (uint64_t) durata_us / 1000ULL);
}

/// Calitatea fisierului MP3 (kbps), citita din antetul primului cadru.
///
/// De ce ne trebuie: serverul nostru trebuie sa livreze la ritmul muzicii, ca un post de radio,
/// nu cat de repede poate. Ca sa stie cat inseamna "ritmul muzicii", trebuie sa afle calitatea.
/// Intoarce 0 daca nu recunoaste antetul; atunci nu franam deloc.
unsigned mp3_kbps(FILE *fisier) {
  static const uint16_t MPEG1_L3[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static const uint16_t MPEG2_L3[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

  // ATENTIE la marimea buffer-ului de mai jos: functia asta ruleaza pe sarcina serverului web,
  // care are o stiva de cativa kilobyti. Prima varianta folosea un vector local de 4096 octeti
  // si depasea stiva - placa se reseta cu "Guru Meditation Error" de fiecare data cand pornea o
  // melodie. Aici tinem buffer-ul mic dinadins si sarim peste eticheta cu fseek, nu citind-o.
  uint8_t cap[256];

  if (fread(cap, 1, 10, fisier) != 10) {
    fseek(fisier, 0, SEEK_SET);
    return 0;
  }

  long inceput_audio = 0;
  // Eticheta ID3v2 de la inceput poate avea si sute de kilobyti (coperta albumului). Nu o
  // citim - ii aflam doar marimea si sarim peste ea. Marimea sta pe 4 octeti din care se
  // foloseste doar cate 7 biti - asa e formatul.
  if (cap[0] == 'I' && cap[1] == 'D' && cap[2] == '3') {
    const long marime = ((long) (cap[6] & 0x7F) << 21) | ((long) (cap[7] & 0x7F) << 14) |
                        ((long) (cap[8] & 0x7F) << 7) | (long) (cap[9] & 0x7F);
    inceput_audio = 10 + marime;
  }

  if (fseek(fisier, inceput_audio, SEEK_SET) != 0) {
    fseek(fisier, 0, SEEK_SET);
    return 0;
  }

  const size_t citit = fread(cap, 1, sizeof(cap), fisier);
  fseek(fisier, 0, SEEK_SET);

  for (size_t i = 0; citit >= 4 && i + 3 < citit; i++) {
    if (cap[i] != 0xFF || (cap[i + 1] & 0xE0) != 0xE0) {
      continue;  // nu e inceput de cadru
    }
    const uint8_t versiune = (cap[i + 1] >> 3) & 0x03;  // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    const uint8_t strat = (cap[i + 1] >> 1) & 0x03;     // 1 = Layer III
    const uint8_t index = (cap[i + 2] >> 4) & 0x0F;
    if (strat != 1 || versiune == 1 || index == 0 || index == 15) {
      continue;  // antet fara sens - cautam mai departe
    }
    return (versiune == 3) ? MPEG1_L3[index] : MPEG2_L3[index];
  }
  return 0;
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
  // Serverul se porneste in loop(), nu aici - vezi explicatia de la declaratia lui loop().
}

void SDCard::loop() {
  if (!this->server_pornit_) {
    this->server_pornit_ = true;
    this->start_http_server_();
  }
  // Nu mai avem nimic de facut in bucla - o oprim, ca sa nu incarcam degeaba placa.
  this->disable_loop();
}

void SDCard::start_http_server_() {
  if (this->http_port_ == 0) {
    return;
  }

  // Rezerva de citire o cerem intai din memoria externa (PSRAM), ca sa nu consumam din cea
  // interna, care e la 74%. Daca nu e PSRAM, se ia din cea interna.
  RAMAllocator<uint8_t> allocator{RAMAllocator<uint8_t>::ALLOC_EXTERNAL};
  this->buffer_ = allocator.allocate(BUFFER_SIZE);
  if (this->buffer_ == nullptr) {
    // Fara PSRAM disponibila, incercam si memoria interna - dar cu o rezerva mai mica.
    RAMAllocator<uint8_t> intern{RAMAllocator<uint8_t>::ALLOC_INTERNAL};
    this->buffer_ = intern.allocate(4096);
    this->buffer_size_ = 4096;
  } else {
    this->buffer_size_ = BUFFER_SIZE;
  }
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Nu am memorie pentru rezerva de citire - serverul nu porneste.");
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->http_port_;
  // ATENTIE: pagina web a placii foloseste deja portul de control implicit (32768). Doua
  // servere cu acelasi port de control nu pornesc - de-aia il mutam pe al nostru.
  config.ctrl_port = 32769;
  // PRIORITATEA - aici era buba, si e cel mai important rand din tot fisierul.
  // Serverul web al ESP-IDF porneste implicit cu prioritatea 5. Dar decodorul de MP3 al
  // ESPHome (cel care trebuie sa tina pasul cu muzica, fara nicio pauza) ruleaza cu
  // prioritatea 1 - verificat in speaker_media_player.cpp, MEDIA_PIPELINE_TASK_PRIORITY = 1.
  // Adica serverul nostru era de cinci ori mai important decat decodorul si il dadea la o
  // parte de fiecare data cand avea ceva de trimis, adica tot timpul. De-aia radioul mergea
  // curat (datele vin din afara, nu exista niciun server pe placa) si de-aia n-a ajutat nimic
  // din ce am oprit: egalizator, leduri, pagina web - toate stau si ele pe prioritate mica.
  // Pus pe 1, serverul si decodorul isi impart procesorul in mod egal, pe rand.
  config.task_priority = 1;
  config.max_open_sockets = 2;
  // 6 KB, nu 4: citirea prin sistemul de fisiere consuma si ea din stiva sarcinii, iar noi mai
  // avem si buffere locale in tratarea cererii. Cu 4 KB eram prea aproape de margine.
  config.stack_size = 6144;
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

  // FRANA - asta e miezul reparatiei. Fara ea serverul toarna fisierul cat de repede poate, iar
  // sarcina care citeste nu mai doarme niciodata si se cearta cu decodorul pe procesor (au
  // aceeasi prioritate). Un post de radio nu face asta: el livreaza la ritmul muzicii, iar
  // cititorul doarme intre pachete. Aici ne purtam la fel.
  const unsigned calitate_kbps = mp3_kbps(fisier);
  // Octeti pe secunda, cu doar 5% peste ritmul real. Marja trebuie sa fie MICA dinadins: daca
  // livram mult mai repede decat se consuma, rezerva playerului se umple, iar cititorul lui
  // incepe iar sa se invarta incercand sa bage date intr-un vas plin - exact starea de care
  // fugim. Cu 5%, rezerva se umple atat de incet incat nu apuca sa se umple pana la finalul
  // melodiei, iar cititorul sta linistit, asteptand date, ca la radio.
  const uint32_t ritm_octeti_pe_sec = (calitate_kbps > 0) ? (uint32_t) (calitate_kbps * 1000 / 8 * 21 / 20) : 0;
  // Primii 512 KB ii trimitem nefranati: sunt vreo 30 de secunde de muzica pusa deoparte, din
  // care playerul porneste imediat si are din ce trai daca placa se impiedica de ceva.
  static constexpr size_t AVANS = 512 * 1024;
  ESP_LOGI(TAG, "Trimit %s: %u kbps, franez la %u KB/s dupa primii %u KB.", nume.c_str(), calitate_kbps,
           (unsigned) (ritm_octeti_pe_sec / 1024), (unsigned) (AVANS / 1024));

  // MASURATOARE (temporara, pentru intreruperi): cronometram separat citirea de pe card si
  // trimiterea catre player. Daca sunetul se rupe, una din cele doua se opreste undeva.
  // Scriem in log doar cand chiar dureaza mult, altfel am ineca consola.
  // Pragul a fost 120 ms, dar masuratoarea ne-a lamurit ca asteptarile de ~1,4 s la trimitere
  // sunt NORMALE: playerul consuma la ritmul muzicii, iar noi asteptam sa faca loc. Nu e o
  // blocare, e franare naturala. Ridicat la 2,5 s ca sa iasa in log doar opririle adevarate.
  static constexpr int64_t PRAG_US = 2500000;  // 2,5 s
  int64_t maxim_card_us = 0;
  int64_t maxim_retea_us = 0;
  uint32_t bucati = 0;

  const int64_t inceput_us = esp_timer_get_time();
  size_t octeti = 0;
  // Momentul si numarul de octeti de la care incepe regimul asezat (dupa umplerea rezervelor).
  int64_t asezat_us = 0;
  size_t asezat_octeti = 0;

  size_t citit;
  while (true) {
    int64_t t0 = esp_timer_get_time();
    citit = fread(self->buffer_, 1, self->buffer_size_, fisier);
    int64_t t1 = esp_timer_get_time();
    if (citit == 0) {
      break;
    }

    esp_err_t trimis = httpd_resp_send_chunk(request, (const char *) self->buffer_, citit);
    int64_t t2 = esp_timer_get_time();

    const int64_t card_us = t1 - t0;
    const int64_t retea_us = t2 - t1;
    maxim_card_us = std::max(maxim_card_us, card_us);
    maxim_retea_us = std::max(maxim_retea_us, retea_us);
    bucati++;
    octeti += citit;

    // FRANA: daca am trimis mai mult decat ii trebuie muzicii pana acum, dormim diferenta.
    // Somnul asta e tot rostul: cat dormim noi, decodorul are procesorul numai pentru el.
    if (ritm_octeti_pe_sec > 0 && octeti > AVANS) {
      const int64_t scurs_ms = (t2 - inceput_us) / 1000;
      const int64_t cuvenit = (int64_t) AVANS + (int64_t) ritm_octeti_pe_sec * scurs_ms / 1000;
      if ((int64_t) octeti > cuvenit) {
        const int64_t exces = (int64_t) octeti - cuvenit;
        int64_t somn_ms = exces * 1000 / (int64_t) ritm_octeti_pe_sec;
        if (somn_ms > 500) {
          somn_ms = 500;  // niciodata mai mult de o jumatate de secunda dintr-o data
        }
        if (somn_ms > 0) {
          vTaskDelay(pdMS_TO_TICKS(somn_ms));
        }
      }
    }

    // Dupa 12 secunde consideram ca rezervele s-au umplut si livrarea merge la ritmul muzicii.
    // De aici incolo masuram calitatea reala a fisierului.
    if (asezat_us == 0 && (t2 - inceput_us) > 12000000) {
      asezat_us = t2;
      asezat_octeti = octeti;
    }

    if (card_us > PRAG_US || retea_us > PRAG_US) {
      ESP_LOGW(TAG, "Bucata %u: cititul de pe card %lld ms, trimiterea %lld ms", (unsigned) bucati,
               (long long) (card_us / 1000), (long long) (retea_us / 1000));
    }

    if (trimis != ESP_OK) {
      // Playerul a inchis legatura (ai schimbat melodia, de exemplu) - nu e o eroare.
      fclose(fisier);
      ESP_LOGI(TAG, "Oprit dupa %u bucati (%u KB). Card: cel mai lung %lld ms. "
                    "Trimitere: cea mai lunga %lld ms. Calitate: ~%u kbps (0 = prea scurt "
                    "ca sa pot masura; lasa melodia sa cante 25 de secunde).",
               (unsigned) bucati, (unsigned) (octeti / 1024), (long long) (maxim_card_us / 1000),
               (long long) (maxim_retea_us / 1000), kbps(octeti, asezat_octeti, asezat_us));
      return ESP_FAIL;
    }
  }
  fclose(fisier);

  const unsigned calitate = kbps(octeti, asezat_octeti, asezat_us);
  ESP_LOGI(TAG, "Fisier terminat: %u bucati (%u KB). Card: cel mai lung %lld ms. "
                "Trimitere: cea mai lunga %lld ms. Calitate: ~%u kbps (0 = prea scurt).",
           (unsigned) bucati, (unsigned) (octeti / 1024), (long long) (maxim_card_us / 1000),
           (long long) (maxim_retea_us / 1000), calitate);
  if (calitate > 200) {
    ESP_LOGW(TAG, "Peste 200 kbps - placa are de tras si sunetul se poate rupe. "
                  "Sub 192 kbps merge curat.");
  }

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
