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

  /// @brief Citeste melodia de la indexul dat, INTEGRAL, de pe card in memoria externa (PSRAM),
  /// si intoarce un audio::AudioFile gata de dat direct la media_player-ul de tip "speaker"
  /// prin play_file(). Intoarce nullptr daca indexul e gresit, tipul fisierului nu e recunoscut
  /// (doar MP3/WAV/FLAC/OPUS), fisierul nu se poate deschide, sau nu mai e memorie externa
  /// libera pentru el - in toate cazurile, doar scrie un avertisment in log, nu opreste placa.
  ///
  /// Bufferul intors ramane valabil pana la URMATOAREA chemare a lui load_track() - apelantul
  /// nu trebuie sa il elibereze singur.
  audio::AudioFile *load_track(int index);

  /// @brief Elibereaza bufferul melodiei DINAINTEA celei curente, daca mai e tinut minte unul.
  /// Apeleaz-o dupa ce te-ai asigurat ca melodia noua chiar canta (media_player.is_playing),
  /// ca sa nu tii doua melodii intregi in memorie decat cat dureaza tranzitia. Sigur de apelat
  /// oricand, chiar daca nu e nimic de eliberat.
  void free_previous();

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

  // Bufferul (mare, din memoria externa) cu melodia CURENTA, si cel cu melodia DINAINTEA ei.
  // Cel dinainte NU se elibereaza automat la urmatoarea incarcare - ramane in viata pana cand
  // cineva cheama explicit free_previous() (scriptul YAML o face imediat ce media_player confirma
  // ca melodia noua chiar canta). Asa tinem fereastra in care exista doua melodii intregi simultan
  // in memorie cat mai scurta posibil - doar tranzitia, nu o melodie intreaga.
  uint8_t *buffer_acum_{nullptr};
  uint8_t *buffer_dinainte_{nullptr};
  audio::AudioFile fisier_curent_{};
};

}  // namespace esphome::sdcard

#endif
