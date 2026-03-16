#pragma once

#include <cstdint>
#include <memory>
#include <list>

// Forward declaration
class Order;

// Type aliases
using Price = std::int32_t; // For readability
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;
using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;