#pragma once

#include <memory>
#include <list>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include "OrderType.h"
#include "Constants.h"

// Forward declarations
using Price    = int64_t;
using Quantity = int64_t;
using OrderId = std::uint64_t;

class Order {
    // represents one individual order
public:
    Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
        : orderType_{orderType}
          , orderId_{orderId}
          , side_{side}
          , price_{price}
          , initialQuantity_{quantity}
          , remainingQuantity_{quantity} {
        if (quantity == 0) {
            throw std::invalid_argument("Order quantity must be greater than zero");
        }
    }

    Order(OrderId orderId, Side side, Quantity quantity)
        : Order(OrderType::Market, orderId, side, Constants::InvalidPrice, quantity) {
    }

    OrderId GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    OrderType GetOrderType() const { return orderType_; }
    Quantity GetInitialQuantity() const { return initialQuantity_; }
    Quantity GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity GetFilledQuantity() const { return GetInitialQuantity() - GetRemainingQuantity(); }
    bool IsFilled() const { return GetRemainingQuantity() == 0; }

    void Fill(Quantity quantity) {
        // Fills part of an order
        if (quantity > GetRemainingQuantity()) {
            std::ostringstream oss;
            oss << "Order (" << GetOrderId() 
                << ") cannot be filled for more than its remaining quantity.";
            throw std::logic_error(oss.str());
        }
        remainingQuantity_ -= quantity;
    }

    void ToGoodTillCancel(Price price) {
        if (orderType_ != OrderType::Market) {
            throw std::logic_error("Cannot convert non-market order to GoodTillCancel");
        }

        price_ = price;
        orderType_ = OrderType::GoodTillCancel;
    }

    // Methods for memory pool reuse (internal use only)
    void reinitialize(OrderType orderType, OrderId orderId, Side side,
                      Price price, Quantity quantity) {
        orderType_ = orderType;
        orderId_ = orderId;
        side_ = side;
        price_ = price;
        initialQuantity_ = quantity;
        remainingQuantity_ = quantity;
    }

    void reset_for_reuse() {
        orderType_ = OrderType::GoodTillCancel;
        orderId_ = 0;
        side_ = Side::Buy;
        price_ = 0;
        initialQuantity_ = 0;
        remainingQuantity_ = 0;
    }

private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity initialQuantity_;
    Quantity remainingQuantity_;
};