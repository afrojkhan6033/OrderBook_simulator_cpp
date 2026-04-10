#pragma once

#include <cstdint>
#include <memory>
#include <list>

// Forward declaration
class Order;

// Type aliases
using Price    = int64_t;
using Quantity = int64_t;
using OrderId = std::uint64_t;
using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;