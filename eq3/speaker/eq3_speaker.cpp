#include "eq3_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome::eq3 {

static const char *const TAG = "eq3_speaker";

// How much audio is filtered and forwarded at a time. Small enough to keep latency low, large enough
// to keep the per-call overhead reasonable.
static const uint32_t PROCESS_BUFFER_DURATION_MS = 20;

// Upper bound on how long a single play() call is allowed to block downstream, regardless of what
// ticks_to_wait the caller (e.g. the mixer task) passes in. EQ3 is just a filter-and-forward passthrough
// in the middle of the audio chain - it must never be the reason a caller's task stalls long enough to
// trip the watchdog, even if the physical speaker or its I2C control bus is temporarily stuck.
static const TickType_t MAX_OUTPUT_TICKS_TO_WAIT = pdMS_TO_TICKS(50);

static const float BASS_FREQUENCY_HZ = 300.0f;
static const float MID_FREQUENCY_HZ = 1000.0f;
static const float MID_Q = 0.8f;
static const float TREBLE_FREQUENCY_HZ = 3000.0f;
static const float SHELF_SLOPE = 1.0f;

// The three biquad designs below follow the Audio EQ Cookbook (Robert Bristow-Johnson) formulas for a
// low shelf, high shelf, and peaking (bell) filter. Coefficients are normalized so a0 == 1.

static BiquadCoeffs make_low_shelf(float frequency_hz, float gain_db, float slope, uint32_t sample_rate) {
  const float a = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * static_cast<float>(M_PI) * frequency_hz / static_cast<float>(sample_rate);
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float alpha = sin_w0 / 2.0f * sqrtf((a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f);
  const float two_sqrt_a_alpha = 2.0f * sqrtf(a) * alpha;

  const float b0 = a * ((a + 1.0f) - (a - 1.0f) * cos_w0 + two_sqrt_a_alpha);
  const float b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cos_w0);
  const float b2 = a * ((a + 1.0f) - (a - 1.0f) * cos_w0 - two_sqrt_a_alpha);
  const float a0 = (a + 1.0f) + (a - 1.0f) * cos_w0 + two_sqrt_a_alpha;
  const float a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cos_w0);
  const float a2 = (a + 1.0f) + (a - 1.0f) * cos_w0 - two_sqrt_a_alpha;

  return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

static BiquadCoeffs make_high_shelf(float frequency_hz, float gain_db, float slope, uint32_t sample_rate) {
  const float a = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * static_cast<float>(M_PI) * frequency_hz / static_cast<float>(sample_rate);
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float alpha = sin_w0 / 2.0f * sqrtf((a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f);
  const float two_sqrt_a_alpha = 2.0f * sqrtf(a) * alpha;

  const float b0 = a * ((a + 1.0f) + (a - 1.0f) * cos_w0 + two_sqrt_a_alpha);
  const float b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cos_w0);
  const float b2 = a * ((a + 1.0f) + (a - 1.0f) * cos_w0 - two_sqrt_a_alpha);
  const float a0 = (a + 1.0f) - (a - 1.0f) * cos_w0 + two_sqrt_a_alpha;
  const float a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cos_w0);
  const float a2 = (a + 1.0f) - (a - 1.0f) * cos_w0 - two_sqrt_a_alpha;

  return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

static BiquadCoeffs make_peaking(float frequency_hz, float q, float gain_db, uint32_t sample_rate) {
  const float a = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * static_cast<float>(M_PI) * frequency_hz / static_cast<float>(sample_rate);
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float alpha = sin_w0 / (2.0f * q);

  const float b0 = 1.0f + alpha * a;
  const float b1 = -2.0f * cos_w0;
  const float b2 = 1.0f - alpha * a;
  const float a0 = 1.0f + alpha / a;
  const float a1 = -2.0f * cos_w0;
  const float a2 = 1.0f - alpha / a;

  return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Direct Form I biquad step: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2.
static inline float apply_biquad(float x, const BiquadCoeffs &c, BiquadState &s) {
  const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
  s.x2 = s.x1;
  s.x1 = x;
  s.y2 = s.y1;
  s.y1 = y;
  return y;
}

void EQ3Speaker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "EQ3 Speaker:\n"
                "  Bass Gain: %.1f dB (~%.0f Hz shelf)\n"
                "  Mid Gain: %.1f dB (~%.0f Hz bell)\n"
                "  Treble Gain: %.1f dB (~%.0f Hz shelf)\n"
                "  Headroom attenuation: %.1f dB",
                this->bass_gain_db_, BASS_FREQUENCY_HZ, this->mid_gain_db_, MID_FREQUENCY_HZ,
                this->treble_gain_db_, TREBLE_FREQUENCY_HZ, 20.0f * log10f(this->pre_gain_));
}

void EQ3Speaker::setup() {}

void EQ3Speaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (this->output_speaker_->is_running()) {
        this->state_ = speaker::STATE_RUNNING;
      }
      break;
    case speaker::STATE_RUNNING:
      if (this->output_speaker_->is_stopped()) {
        this->state_ = speaker::STATE_STOPPED;
      }
      break;
    case speaker::STATE_STOPPING:
      if (this->output_speaker_->is_stopped()) {
        this->state_ = speaker::STATE_STOPPED;
      }
      break;
    case speaker::STATE_STOPPED:
    default:
      break;
  }
}

void EQ3Speaker::start() {
  if (!this->is_stopped()) {
    return;
  }

  this->output_speaker_->set_audio_stream_info(this->audio_stream_info_);

  const size_t frame_size = this->audio_stream_info_.frames_to_bytes(1);
  size_t wanted_size = this->audio_stream_info_.ms_to_bytes(PROCESS_BUFFER_DURATION_MS);
  if (frame_size > 0) {
    wanted_size = (wanted_size / frame_size) * frame_size;
  }
  if (wanted_size == 0) {
    wanted_size = (frame_size > 0) ? frame_size : 256;
  }
  if (this->process_buffer_size_ != wanted_size) {
    this->process_buffer_ = std::make_unique<uint8_t[]>(wanted_size);
    this->process_buffer_size_ = wanted_size;
  }

  this->update_pre_gain_();
  this->update_bass_coeffs_();
  this->update_mid_coeffs_();
  this->update_treble_coeffs_();

  for (uint8_t ch = 0; ch < EQ3_MAX_CHANNELS; ch++) {
    this->bass_state_[ch] = BiquadState{};
    this->mid_state_[ch] = BiquadState{};
    this->treble_state_[ch] = BiquadState{};
  }

  this->output_speaker_->start();
  this->state_ = speaker::STATE_STARTING;
}

void EQ3Speaker::stop() {
  for (uint8_t ch = 0; ch < EQ3_MAX_CHANNELS; ch++) {
    this->level_[ch] = 0.0f;
    this->rms_level_[ch] = 0.0f;
  }
  this->output_speaker_->stop();
  this->state_ = speaker::STATE_STOPPING;
}

void EQ3Speaker::finish() {
  this->output_speaker_->finish();
  this->state_ = speaker::STATE_STOPPING;
}

void EQ3Speaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->output_speaker_->set_mute_state(mute_state);
}

void EQ3Speaker::set_volume(float volume) {
  this->volume_ = volume;
  this->output_speaker_->set_volume(volume);
}

void EQ3Speaker::set_bass_gain_db(float gain_db) {
  this->bass_gain_db_ = gain_db;
  this->update_pre_gain_();
  if (this->audio_stream_info_.get_sample_rate() > 0) {
    this->update_bass_coeffs_();
  }
}

void EQ3Speaker::set_mid_gain_db(float gain_db) {
  this->mid_gain_db_ = gain_db;
  this->update_pre_gain_();
  if (this->audio_stream_info_.get_sample_rate() > 0) {
    this->update_mid_coeffs_();
  }
}

void EQ3Speaker::set_treble_gain_db(float gain_db) {
  this->treble_gain_db_ = gain_db;
  this->update_pre_gain_();
  if (this->audio_stream_info_.get_sample_rate() > 0) {
    this->update_treble_coeffs_();
  }
}

void EQ3Speaker::update_pre_gain_() {
  const float largest_boost_db = std::max(0.0f, std::max(this->bass_gain_db_, std::max(this->mid_gain_db_, this->treble_gain_db_)));
  this->pre_gain_ = powf(10.0f, -largest_boost_db / 20.0f);
}

void EQ3Speaker::update_bass_coeffs_() {
  this->bass_coeffs_ = make_low_shelf(BASS_FREQUENCY_HZ, this->bass_gain_db_, SHELF_SLOPE,
                                      this->audio_stream_info_.get_sample_rate());
}

void EQ3Speaker::update_mid_coeffs_() {
  this->mid_coeffs_ =
      make_peaking(MID_FREQUENCY_HZ, MID_Q, this->mid_gain_db_, this->audio_stream_info_.get_sample_rate());
}

void EQ3Speaker::update_treble_coeffs_() {
  this->treble_coeffs_ = make_high_shelf(TREBLE_FREQUENCY_HZ, this->treble_gain_db_, SHELF_SLOPE,
                                         this->audio_stream_info_.get_sample_rate());
}

float EQ3Speaker::process_sample_(float x, uint8_t channel) {
  x *= this->pre_gain_;
  x = apply_biquad(x, this->bass_coeffs_, this->bass_state_[channel]);
  x = apply_biquad(x, this->mid_coeffs_, this->mid_state_[channel]);
  x = apply_biquad(x, this->treble_coeffs_, this->treble_state_[channel]);
  return std::min(std::max(x, -1.0f), 0.999999f);
}

size_t EQ3Speaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }

  ticks_to_wait = std::min(ticks_to_wait, MAX_OUTPUT_TICKS_TO_WAIT);

  if (!this->output_speaker_->is_running() || this->process_buffer_ == nullptr) {
    // Not ready yet - the caller is expected to retry, matching the play() contract. Crucially, actually
    // wait out ticks_to_wait here (mirroring the mixer's own SourceSpeaker::play() "not ready" branch,
    // which does the same via vTaskDelay) instead of returning instantly. A caller that retries in a
    // tight loop - like the mixer's dedicated task, which runs at a HIGHER FreeRTOS priority than the
    // main loop task - would otherwise spin at full CPU speed with no yield at all every time this
    // branch is hit, starving lower-priority tasks (including the main loop task that feeds the task
    // watchdog) long enough to trip it. This is the real root cause of the "mixer" task watchdog reboot:
    // it reproduces any time the output speaker briefly reports not-running (e.g. while the i2s pipeline
    // settles around a Bluetooth on/off transition), even with no real audio flowing at all - the
    // earlier single-chunk and 50ms-clamp fixes narrowed the window but never closed this fast path.
    vTaskDelay(ticks_to_wait);
    return 0;
  }

  const uint8_t bytes_per_sample = this->audio_stream_info_.get_bits_per_sample() / 8;
  const uint8_t channels = this->audio_stream_info_.get_channels();
  const size_t frame_size = static_cast<size_t>(bytes_per_sample) * channels;

  if (bytes_per_sample == 0 || channels == 0 || channels > EQ3_MAX_CHANNELS) {
    // Unexpected/unsupported format - pass through unfiltered rather than dropping audio.
    return this->output_speaker_->play(data, length, ticks_to_wait);
  }

  // Filter and forward at most one process_buffer_-sized, frame-aligned chunk per call - never loop here
  // calling output_speaker_->play() repeatedly. Each of those calls can block for up to ticks_to_wait
  // (e.g. waiting for I2S DMA buffer space); looping let a single caller's play() call (the mixer, on its
  // own task) block for a multiple of that, long enough to starve its task loop and trip the watchdog -
  // this is what caused the "mixer" task watchdog reboot once real audio started flowing. Returning a
  // partial count here is normal Speaker::play() contract: the caller retries with the remainder.
  size_t chunk = std::min(length, this->process_buffer_size_);
  chunk = (chunk / frame_size) * frame_size;
  if (chunk == 0) {
    // Less than one frame was offered - nothing to do yet. Wait out ticks_to_wait rather than returning
    // instantly, for the same reason as the "not ready" branch above: a tight-looping caller must not be
    // able to spin here with zero yield.
    vTaskDelay(ticks_to_wait);
    return 0;
  }

  uint8_t *dst = this->process_buffer_.get();
  // Measured per channel, so a stereo VU meter can show left and right independently.
  float block_peak[EQ3_MAX_CHANNELS] = {};
  float sum_of_squares[EQ3_MAX_CHANNELS] = {};
  size_t sample_count[EQ3_MAX_CHANNELS] = {};
  for (size_t offset = 0; offset < chunk; offset += bytes_per_sample) {
    const uint8_t channel = (offset / bytes_per_sample) % channels;
    const int32_t sample_q31 = audio::unpack_audio_sample_to_q31(data + offset, bytes_per_sample);
    float sample_f = static_cast<float>(sample_q31) / 2147483648.0f;
    sample_f = this->process_sample_(sample_f, channel);
    block_peak[channel] = std::max(block_peak[channel], std::fabs(sample_f));
    sum_of_squares[channel] += sample_f * sample_f;
    ++sample_count[channel];
    const int32_t out_q31 = static_cast<int32_t>(sample_f * 2147483648.0f);
    audio::pack_q31_as_audio_sample(out_q31, dst + offset, bytes_per_sample);
  }
  // Publish both numbers a VU meter needs, for each channel: peak and average energy.
  for (uint8_t ch = 0; ch < EQ3_MAX_CHANNELS; ch++) {
    this->level_[ch] = block_peak[ch];
    this->rms_level_[ch] =
        (sample_count[ch] > 0) ? std::sqrt(sum_of_squares[ch] / static_cast<float>(sample_count[ch])) : 0.0f;
  }
  // A mono stream only fills channel 0 - mirror it so both halves of a stereo display light up.
  if (channels == 1) {
    for (uint8_t ch = 1; ch < EQ3_MAX_CHANNELS; ch++) {
      this->level_[ch] = this->level_[0];
      this->rms_level_[ch] = this->rms_level_[0];
    }
  }

  return this->output_speaker_->play(dst, chunk, ticks_to_wait);
}

}  // namespace esphome::eq3

#endif
