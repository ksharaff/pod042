#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// The last N values of something, oldest first. When it is full, pushing a new
// value drops the oldest one (a "ring buffer"). Used for the sparklines.
class History {
public:
    explicit History(std::size_t capacity) : capacity_(capacity) {}

    void push(double value);
    void clear() { data_.clear(); }
    std::size_t size() const { return data_.size(); }

    // A copy of the values, oldest first.
    std::vector<double> values() const;

private:
    std::size_t capacity_;
    std::deque<double> data_;   // a deque is cheap to remove from at the front
};


std::string sparkline(const std::vector<double>& values, std::size_t width,
                      double minValue, double maxValue);

// Same, but the scale runs from 0 to the largest value currently shown.
// Used for speeds, which have no natural maximum.
std::string sparklineAuto(const std::vector<double>& values, std::size_t width);