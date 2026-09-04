#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/audio/audio.h"

#include <string>
#include <vector>

namespace esphome::sdcard {

// Monteaza cardul SD (mod 1 fir) si citeste melodiile de pe el, integral, in memoria externa
// (PSRAM), la cerere - vezi load_track(). NU mai exista niciun server web: fisierul citit e dat
// DIRECT playerului prin SpeakerMediaPlayer::play_file(), aceeasi metoda publica pe care o
// foloseste si componenta oficiala "audio_file" a ESPHome-ului pentru sunete din firmware.
class SDCard : public Component {
 public:
  // DATA: cardul trebuie montat devreme, ca sa fie gata cand alte componente il cauta.
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;
  void dump_config() override;

  void set_mount_point(const std::string &mount_point) { this->mount_point_ = mount_point; }
  void set_music_folder(const std::string &music_folder) { this->music_folder_ = music_folder; }

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

  /// @brief Spune daca melodia de la indexul dat ar incapea in memoria externa DUPA ce am
  /// elibera bufferul curent (daca exista) - o verificare ieftina (doar marimea fisierului pe
  /// disc, fara sa il citim). Foloseste cel mai mare bloc CONTINUU liber, nu doar totalul - un
  /// fisier poate sa nu incapa chiar daca suma bucatilor libere ar ajunge, daca memoria e
  /// fragmentata. Gandita sa fie chemata INAINTE sa oprim redarea curenta (vezi schimba_melodie
  /// din YAML), ca sa nu taiem sunetul degeaba pentru o melodie care oricum nu are cum sa incapa.
  ///
  /// Marja de siguranta folosita difera dupa cum a mai cantat ceva prin placa in sesiunea asta
  /// (radio, Bluetooth sau o alta melodie) - vezi note_playback_active() si comentariul de la
  /// PIPELINE_OVERHEAD_ACTIV din .cpp pentru explicatia completa.
  bool track_fits(int index) const;

  /// @brief Anunta cardul ca ceva a INCEPUT sa cante prin placa - radio, Bluetooth sau o melodie
  /// de pe card (chemata automat de load_track() la succes; scriptul YAML o cheama explicit si
  /// pentru radio/Bluetooth). Foloseste doar pentru track_fits() - vezi acolo. Sigur de chemat
  /// oricat de des.
  void note_playback_active() { this->a_redat_ceva_ = true; }

  /// @brief Citeste melodia de la indexul dat, INTEGRAL, de pe card in memoria externa (PSRAM),
  /// si intoarce un audio::AudioFile gata de dat direct la media_player-ul de tip "speaker"
  /// prin play_file(). Intoarce nullptr daca indexul e gresit, tipul fisierului nu e recunoscut
  /// (doar MP3/WAV/FLAC/OPUS), fisierul nu se poate deschide, sau nu mai e memorie externa
  /// libera pentru el - in toate cazurile, doar scrie un avertisment in log, nu opreste placa.
  ///
  /// ATENTIE: elibereaza automat bufferul melodiei ANTERIOARE inainte sa aloce cel nou, ca sa
  /// foloseasca tot bugetul de PSRAM disponibil pentru o singura melodie, nu doar jumatate din
  /// el. De-aia APELANTUL trebuie sa se asigure ca redarea melodiei anterioare chiar s-a oprit
  /// (media_player.stop, urmat de o pauza) inainte sa cheme load_track() din nou - altfel risca
  /// sa stearga de sub decodor un buffer inca in folosinta. Vezi schimba_melodie din YAML pentru
  /// tiparul corect (acelasi tipar - stop + pauza - folosit deja la trecerea pe Bluetooth).
  audio::AudioFile *load_track(int index);

  /// @brief Scrie in log, la nivel INFO, cati KB PSRAM sunt liberi (total si cel mai mare bloc
  /// continuu) chiar acum, cu o eticheta data de apelant. Diagnostic pur - nu schimba nimic.
  /// Gandit sa fie presarat prin scripturile YAML (inclusiv cele care NU ating deloc cardul, ca
  /// schimba_post_radio) ca sa vedem exact CAND scade memoria disponibila, nu doar la incarcarea
  /// unei melodii.
  void log_psram(const char *eticheta) const;

 protected:
  std::string mount_point_{"/sd"};
  std::string music_folder_{"/sd"};

  bool mounted_{false};
  uint64_t size_mb_{0};
  char name_[8]{};

  std::vector<std::string> tracks_;

  // Bufferul (mic, din memoria INTERNA, potrivit pentru transfer direct) prin care trec toate
  // citirile de pe card - vezi explicatia mare din load_track() despre de ce nu citim direct in
  // memoria externa. Alocat o singura data, la prima folosire, si refolosit de fiecare data.
  uint8_t *citire_{nullptr};

  // Bufferul (mare, din memoria externa) cu melodia CURENTA. NU mai tinem si melodia dinainte -
  // testele au aratat ca placa are prea putin PSRAM liber (~2,7 MB, chiar si fara nimic de pe
  // card) ca sa incapa doua melodii intregi simultan, chiar si pentru cateva secunde cat dura o
  // tranzitie. Acum load_track() elibereaza mereu bufferul vechi INAINTE sa aloce cel nou, ca
  // fiecare melodie sa poata folosi tot bugetul disponibil.
  uint8_t *buffer_curent_{nullptr};
  audio::AudioFile fisier_curent_{};

  // Vezi note_playback_active() si track_fits() - tine minte daca a mai cantat ceva prin placa
  // in sesiunea asta (de la ultimul boot), ca sa alegem marja de siguranta potrivita.
  bool a_redat_ceva_{false};
};

}  // namespace esphome::sdcard

#endif
