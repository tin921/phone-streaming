// Sample-rate conversion to 16 kHz: windowed-sinc interpolation with a
// table of 256 fractional phases (32 taps), low-passed below the output's
// Nyquist frequency. Works for any input rate (44.1, 48, 96 kHz ...).
#pragma once
#include "common.hpp"

namespace ps {

class Resampler {
    static constexpr int Half = 16;
    static constexpr int Taps = 2 * Half;
    static constexpr int Phases = 256;
    std::vector<float> table;
    std::vector<float> buffer;
    double step = 1;
    double position = 0;
public:
    void init(double inRate, double outRate) {
        step = inRate / outRate;
        // Pass band up to 0.45 of the lower rate; the window does the rest.
        double cutoff = 0.45 * std::min(inRate, outRate) / inRate;
        table.assign(size_t(Phases + 1) * Taps, 0.f);
        for (int p = 0; p <= Phases; ++p) {
            double frac = double(p) / Phases, sum = 0;
            double row[Taps];
            for (int k = 0; k < Taps; ++k) {
                double x = (k - (Half - 1)) - frac;
                double sinc = x == 0 ? 2 * cutoff : std::sin(2 * 3.14159265358979 * cutoff * x) / (3.14159265358979 * x);
                double w = x / (Half + 1);
                double window = std::fabs(w) >= 1 ? 0 : 0.42 + 0.5 * std::cos(3.14159265358979 * w) + 0.08 * std::cos(2 * 3.14159265358979 * w);
                row[k] = sinc * window;
                sum += row[k];
            }
            for (int k = 0; k < Taps; ++k) table[size_t(p) * Taps + k] = float(row[k] / sum);
        }
        buffer.assign(Half - 1, 0.f);
        position = Half - 1;
    }

    // Appends input; writes as many output samples as the input allows.
    void process(const float* in, size_t n, std::vector<float>& out) {
        buffer.insert(buffer.end(), in, in + n);
        for (;;) {
            size_t i = size_t(position);
            if (i + Half >= buffer.size()) break;
            int phase = int((position - double(i)) * Phases + 0.5);
            const float* h = &table[size_t(phase) * Taps];
            const float* x = &buffer[i - (Half - 1)];
            float acc = 0;
            for (int k = 0; k < Taps; ++k) acc += x[k] * h[k];
            out.push_back(acc);
            position += step;
        }
        size_t consumed = size_t(position);
        if (consumed > size_t(Half - 1)) {
            size_t drop = consumed - (Half - 1);
            buffer.erase(buffer.begin(), buffer.begin() + std::ptrdiff_t(drop));
            position -= double(drop);
        }
    }
};

} // namespace ps
