#pragma once
// ============================================================================
// OrderPool.h — Memory Pool for Order objects (Object Pool Pattern)
//
// Purpose: Eliminate frequent heap allocations by pre-allocating Order objects
//          in a pool and reusing them. This reduces:
//   - Memory fragmentation from small allocations
//   - Cache misses (orders stored contiguously)
//   - Allocation latency (O(1) acquire vs heap traversal)
//
// Usage:
//   OrderPool pool(10000);  // Pre-allocate 10000 orders
//   auto order = pool.acquire(orderId, side, price, quantity);
//   // ... use order ...
//   pool.release(order);  // Return to pool for reuse
//
// Thread-safety: NOT thread-safe. Use one pool per thread.
// ============================================================================

#include <memory>
#include <vector>
#include <queue>
#include "Order.h"

class OrderPool {
public:
    // Pre-allocate pool with given capacity
    explicit OrderPool(size_t initial_capacity = 10000) {
        reserved_count_ = initial_capacity;
        expand_pool(initial_capacity);
    }

    ~OrderPool() {
        // Shared pointers automatically clean up
    }

    // Acquire an order from the pool (O(1))
    // Returns nullptr if pool is exhausted (should be rare with proper sizing)
    OrderPointer acquire(OrderType orderType, OrderId orderId, Side side,
                         Price price, Quantity quantity) {
        if (available_.empty()) {
            // Expand pool dynamically if needed
            expand_pool(reserved_count_);
        }

        auto order = available_.front();
        available_.pop();

        // Reinitialize order (avoids construction overhead)
        order->reinitialize(orderType, orderId, side, price, quantity);
        active_orders_++;
        return order;
    }

    // Convenience overload for market orders
    OrderPointer acquire(OrderId orderId, Side side, Quantity quantity) {
        return acquire(OrderType::Market, orderId, side,
                      Constants::InvalidPrice, quantity);
    }

    // Release an order back to the pool (O(1))
    void release(OrderPointer order) {
        if (order) {
            order->reset_for_reuse();
            available_.push(order);
            active_orders_--;
        }
    }

    // Statistics
    size_t total_capacity() const { return capacity_; }
    size_t available_count() const { return available_.size(); }
    size_t active_count() const { return active_orders_.load(); }
    double utilization() const {
        return capacity_ > 0 ? static_cast<double>(active_orders_.load()) / capacity_ : 0.0;
    }

    // Pre-warm: expand pool to at least N orders
    void reserve(size_t count) {
        if (available_.size() < count) {
            expand_pool(count - available_.size());
        }
    }

private:
    void expand_pool(size_t additional_count) {
        for (size_t i = 0; i < additional_count; ++i) {
            // Create orders with dummy values, will be reinitialized on acquire
            auto order = std::make_shared<Order>(
                OrderType::GoodTillCancel,
                0, Side::Buy, 0, 1
            );
            pool_.push_back(order);
            available_.push(order);
        }
        capacity_ += additional_count;
    }

    std::vector<OrderPointer> pool_;  // All allocated orders
    std::queue<OrderPointer> available_;  // Orders available for use
    size_t capacity_ = 0;  // Total pool size
    std::atomic<size_t> active_orders_{0};  // Currently in-use orders
    size_t reserved_count_ = 0;  // Target capacity
};

// ============================================================================
// Order reuse optimization — avoids reconstruction overhead
// ============================================================================

// Add these methods to the Order class (we'll add them via Edit)
