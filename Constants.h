#pragma once

#include <limits>
#include <cstdint>

using Price = std::int32_t;

struct Constants {
    static const Price InvalidPrice = std::numeric_limits<Price>::min();
};