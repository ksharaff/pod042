#include "history.h"
#include <algorithm>   // std::min, std::max

void History::push(double value) {
    data_.push_back(value);
    while (data_.size() > capacity_) {
        data_.pop_front();   // forget the oldest value
    }
}

std::vector<double> History::values() const {
    return std::vector<double>(data_.begin(), data_.end());
}

std::string sparkline(const std::vector<double>& values, std::size_t width,
                      double minValue, double maxValue) {
    // The eight "lower block" characters, from 1/8 tall to full height.
    // "\u2581" is how C++ writes the Unicode character U+2581 inside a string.
    static const char* const kBlocks[8] = {
        "\u2581", "\u2582", "\u2583", "\u2584", "\u2585", "\u2586", "\u2587", "\u2588"};

    const std::size_t shown = std::min(values.size(), width);

    std::string out;
    out.append(width - shown, ' ');   // left padding: newest value stays at the right edge

    for (std::size_t i = values.size() - shown; i < values.size(); ++i) {
        double fraction = 0.0;   // 0.0 = bottom of the scale, 1.0 = top
        if (maxValue > minValue) {
            fraction = (values[i] - minValue) / (maxValue - minValue);
        }
        // Clamp to 0..1. Written as "!(x >= 0)" on purpose: it is also true for NaN,
        // so a NaN (not-a-number) value falls back to the lowest block.
        if (!(fraction >= 0.0)) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;

        // Map 0..1 onto the 8 blocks (index 0..7). +0.5 rounds to the nearest one.
        const std::size_t level = static_cast<std::size_t>(fraction * 7.0 + 0.5);
        out += kBlocks[level];
    }
    return out;
}

std::string sparklineAuto(const std::vector<double>& values, std::size_t width) {
    // Only the values that will actually be shown decide the scale, so an old
    // spike that already scrolled off doesn't keep the graph flat.
    const std::size_t start = values.size() > width ? values.size() - width : 0;
    double highest = 0.0;
    for (std::size_t i = start; i < values.size(); ++i) {
        highest = std::max(highest, values[i]);
    }
    return sparkline(values, width, 0.0, highest);
}