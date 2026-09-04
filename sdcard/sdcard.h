#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"

#include <esp_http_server.h>

#include <string>
#include <vector>

namespace esphome::sdcard {

// Monteaza cardul SD (mod 1 fir) si porneste un server web care serveste fisierele de pe el.
// Playerul obisnuit le cere apoi ca pe un post de radio, prin http://127.0.0.1:<port>/<fisier>.
class SDCard : public Component {
 public:
  // DATA: cardul trebuie montat devreme, ca sa fie gata cand alte componente il cauta.
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;
  // Serverul web NU se poate porni din setup(): componenta noastra ruleaza cu prioritatea DATA
  // (600), iar WiFi-ul abia cu 250 - adica DUPA noi. Pornit inainte ca reteaua sa existe,
  // serverul incearca sa deschida socket-uri intr-o stiva neinitializata si placa se reseteaza.
  // In loop() suntem siguri ca tot setup-ul s-a terminat, retea inclusa.
  void loop() override;
  void dump_config() override;

  void set_mount_point(const std::string &mount_point) { this->mount_point_ = mount_point; }
  void set_music_folder(const std::string &music_folder) { this->music_folder_ = music_folder; }
  void set_http_port(uint16_t http_port) { this->http_port_ = http_port; }

  bool is_mounted() const { return this->mounted_; }

  /// @brief Scrie in log tot ce gaseste in folderul dat (maxim 100 de intrari).
  void list_files(const std::string &path);

  /// @brief Reciteste folderul de muzica si reface lista de melodii. Apeleaz-o dupa ce ai
  /// copiat fisiere noi pe card fara sa repornesti placa. Scrie in log doar numarul gasit.
  void scan_music();

  /// @brief Scrie in log lista completa de melodii. Separata de scan_music dinadins: in setup()
  /// zeci de linii de log una dupa alta ingroapa consola si ascund ce e important.
  void log_tracks();

  /// @brief Cate melodii sunt in lista.
  int track_count() const { return (int) this->tracks_.size(); }

  /// @brief Numele melodiei (fara cale), pentru afisat. Sir gol daca indexul e gresit.
  std::string track_name(int index) const;

  /// @brief Adresa completa pe care i-o dai playerului. Sir gol daca indexul e gresit.
  std::string track_url(int index) const;

  /// @brief Inchide imediat, din afara, orice transfer de fisier ramas activ pe server.
  ///
  /// De ce ne trebuie: cand utilizatorul trece de pe card pe alta sursa (radio sau Bluetooth),
  /// playerul deschide o conexiune noua, dar noi aflam ca cea veche s-a inchis abia la
  /// urmatoarea incercare de trimitere - uneori la cateva secunde distanta. Pana atunci, sarcina
  /// serverului nostru (cu bufferul si stiva ei, memorie interna DMA) ramane alocata exact cand
  /// noua sursa are nevoie sa-si realoce propriile bufere DMA pentru difuzor - cele doua se pot
  /// calca pe picioare. Aici inchidem noi conexiunea, dinadins, inainte sa apuce sa se ceara.
  /// Nu face nimic daca nu exista niciun transfer activ.
  void stop_transfer();

 protected:
  void start_http_server_();
  /// @brief Trimite fisierul cerut, bucata cu bucata. Ruleaza pe sarcina serverului web.
  static esp_err_t http_handler_(httpd_req_t *request);

  std::string mount_point_{"/sd"};
  std::string music_folder_{"/sd"};
  uint16_t http_port_{81};

  bool mounted_{false};
  uint64_t size_mb_{0};
  char name_[8]{};

  bool server_pornit_{false};
  httpd_handle_t server_{nullptr};
  uint8_t *buffer_{nullptr};
  size_t buffer_size_{0};

  std::vector<std::string> tracks_;
};

}  // namespace esphome::sdcard

#endif
