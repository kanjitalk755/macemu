/*
 * Tone Generator for Audio Testing
 *
 * Generates test audio signals (sine waves) for testing the audio pipeline
 * without needing the emulator to produce audio.
 */

#ifndef TONE_GENERATOR_H
#define TONE_GENERATOR_H

#include <cstdint>
#include <cmath>
#include <vector>

class ToneGenerator {
public:
    ToneGenerator(int sample_rate = 48000, int channels = 2)
        : sample_rate_(sample_rate)
        , channels_(channels)
        , phase_(0.0)
        , frequency_(440.0)  // A4 note
    {}

    // Set tone frequency in Hz
    void set_frequency(double freq) {
        frequency_ = freq;
    }

    // Get current frequency
    double get_frequency() const {
        return frequency_;
    }

    // Generate audio samples (16-bit PCM)
    // Returns vector of interleaved samples (L,R,L,R,... for stereo)
    std::vector<int16_t> generate_samples(int num_samples) {
        std::vector<int16_t> samples;
        samples.reserve(num_samples * channels_);

        const double phase_increment = 2.0 * M_PI * frequency_ / sample_rate_;
        const int16_t amplitude = 8192;  // ~25% volume to avoid clipping

        for (int i = 0; i < num_samples; i++) {
            // Generate sine wave sample
            double sample_value = std::sin(phase_) * amplitude;
            int16_t sample = static_cast<int16_t>(sample_value);

            // Duplicate for all channels
            for (int ch = 0; ch < channels_; ch++) {
                samples.push_back(sample);
            }

            // Advance phase
            phase_ += phase_increment;
            if (phase_ >= 2.0 * M_PI) {
                phase_ -= 2.0 * M_PI;
            }
        }

        return samples;
    }

    // Generate audio frame at 20ms (standard for Opus)
    std::vector<int16_t> generate_frame() {
        int samples_per_frame = (sample_rate_ * 20) / 1000;  // 20ms
        return generate_samples(samples_per_frame);
    }

    // Reset phase (useful when changing frequency)
    void reset_phase() {
        phase_ = 0.0;
    }

    // Get sample rate
    int get_sample_rate() const {
        return sample_rate_;
    }

    // Get channels
    int get_channels() const {
        return channels_;
    }

private:
    int sample_rate_;
    int channels_;
    double phase_;
    double frequency_;
};

#endif // TONE_GENERATOR_H
