#pragma once
#include <limits>
#include <cstdint>

using Price    = int64_t;
using Quantity = int64_t;

struct Constants {
    static const Price InvalidPrice = std::numeric_limits<Price>::min();
};