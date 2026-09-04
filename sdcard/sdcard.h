#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include <atomic>
#include <cstdio>
#include <memory>
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
  /// @brief Speakerul-sursa (din YAML, ex: "sursa_muzica") catre care play_track_streaming() va
  /// trimite DIRECT sunetul decodat, bucata cu bucata - vezi comentariul mare de la
  /// play_track_streaming() mai jos.
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }

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

  /// @brief NOU - inlocuieste load_track()+play_file() pentru melodiile de pe card. Reda melodia
  /// de la indexul dat "in flux" (streaming): o sarcina (task) dedicata citeste fisierul de pe
  /// card bucata cu bucata (cativa KB o data), le decodeaza pe masura ce vin si trimite sunetul
  /// DIRECT catre speakerul-sursa dat prin set_speaker() (vezi YAML: "speaker: sursa_muzica") -
  /// FARA sa mai treaca prin media_player-ul "audio_player" si FARA sa mai citeasca vreodata
  /// fisierul intreg in memorie mai intai.
  ///
  /// De ce: load_track() (mai sus) cerea tot fisierul deodata in memoria externa (PSRAM) - placa
  /// are doar 4 MB PSRAM fizic, deci fisierele mari (peste ~2-3 MB, dupa cat mai era liber din
  /// cei 4 MB) nu aveau NICIODATA cum sa incapa, oricat am fi ajustat marjele de siguranta.
  /// play_track_streaming() nu mai are limita asta: foloseste un buffer inelar mic (INEL_FLUX,
  /// cativa zeci de KB) prin care trec DOAR bucatile de fisier comprimat citite pe rand, nu tot
  /// fisierul - deci orice melodie, indiferent cat de mare, poate fi acum redata.
  ///
  /// Opreste automat, mai intai, orice flux anterior inca activ (vezi stop_streaming()) - e sigur
  /// sa o chemi oricand, chiar daca ceva canta deja prin ea. Intoarce false daca melodia nu a
  /// putut fi pornita (index gresit, tip de fisier necunoscut, fisier lipsa/ilizibil, sau nu s-a
  /// putut porni sarcina de streaming) - in toate cazurile, doar scrie un avertisment in log.
  bool play_track_streaming(int index);

  /// @brief Opreste fluxul curent (daca exista) si asteapta (cel mult ~2 secunde) ca sarcina lui
  /// interna sa se termine curat singura - vezi comentariul din .cpp pentru cum se face oprirea
  /// fara sa stergem sub ea resurse inca in folosinta. Sigur de chemat si daca nu canta nimic.
  void stop_streaming();

  /// @brief True cat timp play_track_streaming() a pornit o melodie si sarcina ei interna inca
  /// lucreaza (citeste/decodeaza/reda) - folosit din YAML in loc de media_player.is_playing,
  /// pentru ca "audio_player" nu mai stie nimic despre melodiile redate in flux.
  bool is_streaming() const { return this->flux_activ_.load(); }

  /// @brief Pune pe pauza (sau reia) melodia curenta redata in flux - sunetul se opreste/reia
  /// aproape imediat. Sigur de chemat oricand, chiar daca nu canta nimic in flux acum (starea
  /// se aplica automat cand porneste urmatoarea melodie).
  void set_streaming_paused(bool pauza) { this->flux_pauza_.store(pauza); }
  bool is_streaming_paused() const { return this->flux_pauza_.load(); }

  /// @brief True EXACT O SINGURA DATA dupa ce o melodie redata in flux s-a terminat SINGURA,
  /// natural (nu a fost oprita de stop_streaming() - adica nu a fost un skip manual sau o
  /// schimbare de sursa). Gandita sa fie verificata periodic dintr-un "interval:" din YAML, ca
  /// sa se poata trece automat la melodia urmatoare. Citirea "consuma" raspunsul (il pune inapoi
  /// pe false), ca sa nu declansam de doua ori trecerea la urmatoarea melodie pentru acelasi
  /// eveniment.
  bool consume_finished_naturally() { return this->terminat_natural_.exchange(false); }

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

  // --- De aici in jos: doar pentru play_track_streaming()/stop_streaming() (vezi acolo) ---

  // Speakerul-sursa (ex: "sursa_muzica"), dat prin set_speaker() din YAML - tinta finala a
  // sunetului decodat in flux.
  speaker::Speaker *speaker_{nullptr};

  // Bufferul inelar (in memoria externa) prin care sarcina de flux alimenteaza decodorul, in
  // bucati mici - vezi play_track_streaming(). Contine DOAR bytes de fisier comprimat (MP3 etc.),
  // NU audio decodat - de-aia poate fi mic desi melodia intreaga are MB intregi. Detinut aici (nu
  // doar de decodor) ca sa il putem crea INAINTE sa pornim sarcina si sa dam un weak_ptr atat
  // sarcinii cat si decodorului.
  std::shared_ptr<ring_buffer::RingBuffer> inel_flux_;

  // Decodorul folosit de sarcina de flux - creat/distrus o data pe melodie, in
  // play_track_streaming()/in sarcina insasi la final. Atins DOAR din sarcina de flux dupa ce a
  // pornit (niciodata din firul principal in acelasi timp - vezi stop_streaming()).
  std::unique_ptr<audio::AudioDecoder> decodor_flux_;

  // Fisierul curent, deschis in play_track_streaming() (pe firul principal, ca sa putem raporta
  // imediat "fisier lipsa"), dar inchis de sarcina de flux insasi, la final.
  FILE *fisier_flux_{nullptr};

  // true cat timp sarcina de flux e pornita si lucreaza. Sarcina insasi il pune pe false ca ULTIM
  // lucru pe care il face, chiar inainte sa se auto-stearga - vezi stop_streaming() pentru de ce
  // conteaza ordinea asta.
  std::atomic<bool> flux_activ_{false};
  // Cerere de oprire pentru sarcina de flux curenta - pusa pe true de stop_streaming(), citita de
  // sarcina la fiecare bucla a ei. std::atomic pentru ca e citit/scris din doua fire diferite.
  std::atomic<bool> opreste_flux_{false};
  // Cerere de pauza pentru sarcina de flux curenta - vezi set_streaming_paused().
  std::atomic<bool> flux_pauza_{false};
  // Pus pe true de sarcina de flux DOAR cand melodia s-a terminat singura (nu la skip/schimbare
  // de sursa) - vezi consume_finished_naturally().
  std::atomic<bool> terminat_natural_{false};

  // Functia sarcinii (task) FreeRTOS dedicate care citeste+decodeaza+reda o melodie in flux. Vezi
  // .cpp pentru ce face exact, pas cu pas.
  static void flux_task_(void *param);
};

}  // namespace esphome::sdcard

#endif
