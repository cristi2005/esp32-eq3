#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>

#include <memory>

namespace esphome::eq3 {

// One IIR biquad section's coefficients (a0 already normalized to 1, so only b0..b2/a1/a2 are stored).
struct BiquadCoeffs {
  float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
};

// A biquad section's running state (Direct Form I), one instance needed per audio channel.
struct BiquadState {
  float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
};

static const uint8_t EQ3_MAX_CHANNELS = 2;

// A 3-band (bass/mid/treble) software equalizer speaker. Sits between an upstream source (whatever
// calls play() on it - e.g. the mixer) and a downstream output_speaker (e.g. the physical I2S speaker):
// it applies a cascade of 3 biquad filters (low shelf, peaking bell, high shelf) to every sample before
// forwarding it, without changing the sample format.
class EQ3Speaker final : public Component, public speaker::Speaker {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::DATA; }
  void dump_config() override;
  void setup() override;
  void loop() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  void set_pause_state(bool pause_state) override { this->output_speaker_->set_pause_state(pause_state); }
  bool get_pause_state() const override { return this->output_speaker_->get_pause_state(); }

  bool has_buffered_data() const override { return this->output_speaker_->has_buffered_data(); }

  /// @brief Mute state changes are passed to the output speaker.
  void set_mute_state(bool mute_state) override;
  bool get_mute_state() override { return this->output_speaker_->get_mute_state(); }

  /// @brief Volume state changes are passed to the output speaker.
  void set_volume(float volume) override;
  float get_volume() override { return this->output_speaker_->get_volume(); }

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }

  /// @brief Sets the bass (low shelf, ~300 Hz) gain in dB (-12..+12) and recomputes its coefficients.
  void set_bass_gain_db(float gain_db);
  /// @brief Sets the mid (peaking bell, ~1 kHz) gain in dB (-12..+12) and recomputes its coefficients.
  void set_mid_gain_db(float gain_db);
  /// @brief Sets the treble (high shelf, ~3 kHz) gain in dB (-12..+12) and recomputes its coefficients.
  void set_treble_gain_db(float gain_db);

 protected:
  void update_bass_coeffs_();
  void update_mid_coeffs_();
  void update_treble_coeffs_();

  /// @brief Recomputes the safety attenuation applied before filtering. Boosting a band raises the
  /// signal, and audio that is already near full scale would then exceed it and get hard-clipped by
  /// process_sample_(), which is audible as harsh crackling - worst on loud, heavily compressed
  /// material such as rock, while quiet material like classical stays clean. Attenuating the input
  /// by the largest boost first guarantees the cascade can never exceed full scale. The cost is
  /// lower overall loudness, which the user compensates with the volume control.
  void update_pre_gain_();

  /// @brief Runs one sample (normalized to [-1, 1)) through the 3-band cascade for the given channel.
  float process_sample_(float x, uint8_t channel);

  speaker::Speaker *output_speaker_{nullptr};

  // Fixed-size scratch buffer for the filtered audio. Allocated once in start() (never during play()),
  // per the project convention of avoiding heap allocation after setup.
  std::unique_ptr<uint8_t[]> process_buffer_;
  size_t process_buffer_size_{0};

  float bass_gain_db_{0.0f};
  float mid_gain_db_{0.0f};
  float treble_gain_db_{0.0f};

  // Linear factor applied to every sample before the filters. 1.0 when no band is boosted.
  float pre_gain_{1.0f};

  BiquadCoeffs bass_coeffs_;
  BiquadCoeffs mid_coeffs_;
  BiquadCoeffs treble_coeffs_;

  BiquadState bass_state_[EQ3_MAX_CHANNELS];
  BiquadState mid_state_[EQ3_MAX_CHANNELS];
  BiquadState treble_state_[EQ3_MAX_CHANNELS];
};

}  // namespace esphome::eq3

#endif
