#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"

#include <string>

namespace esphome::sdcard {

// Monteaza cardul SD in modul pe 1 fir si il face vizibil ca folder obisnuit (implicit "/sd").
// Etapa 1: doar montare si listare. Redarea vine dupa ce stim sigur ca placa vede cardul.
class SDCard : public Component {
 public:
  // DATA, adica dupa magistrale si inainte de restul: cardul trebuie montat devreme, ca sa fie
  // gata cand alte componente vor sa citeasca de pe el.
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;
  void dump_config() override;

  void set_mount_point(const std::string &mount_point) { this->mount_point_ = mount_point; }

  bool is_mounted() const { return this->mounted_; }
  const std::string &get_mount_point() const { return this->mount_point_; }

  /// @brief Scrie in log tot ce gaseste in folderul dat. Se opreste la 100 de intrari, ca sa nu
  /// blocheze placa pe un card plin. Apeleaz-o dintr-un lambda: id(sd_card).list_files("/sd");
  void list_files(const std::string &path);

 protected:
  std::string mount_point_{"/sd"};
  bool mounted_{false};
  uint64_t size_mb_{0};
  char name_[8]{};
};

}  // namespace esphome::sdcard

#endif

