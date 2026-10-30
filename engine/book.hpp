#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class Tif : uint8_t { Day = 0, IOC = 1, FOK = 2 };
struct OrderId {
    uint64_t v{};
};
struct Order {
    OrderId id{};
    std::string sym;
    Side side{};
    long long px{}, qty{}, ts_ns{};
    Tif tif{Tif::Day};
};
struct Trade {
    OrderId resting_id, incoming_id;
    long long px, qty, seq, ts_ns;
};
struct PriceLevel {
    std::list<Order *> q;
    long long total_qty{};
};
using BidMap = std::map<long long, PriceLevel, std::greater<long long>>;
using AskMap = std::map<long long, PriceLevel>;
struct Book {
    BidMap bids;
    AskMap asks;
    std::unordered_map<uint64_t, std::unique_ptr<Order>> by_id;
    std::unordered_map<uint64_t, std::list<Order *>::iterator> positions;
};
inline void validate_symbol(const std::string &sym) {
    if (sym.empty() || sym.size() > 16)
        throw std::invalid_argument("symbol length must be 1..16");
    for (unsigned char ch : sym)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-'))
            throw std::invalid_argument("invalid symbol character");
}
inline void validate_order(const Order &order) {
    validate_symbol(order.sym);
    if (!order.id.v || (order.side != Side::Buy && order.side != Side::Sell) ||
        (order.tif != Tif::Day && order.tif != Tif::IOC && order.tif != Tif::FOK) || order.px < 0 ||
        order.px > 1000000000000LL || order.qty <= 0 || order.qty > 1000000000LL)
        throw std::invalid_argument("invalid order ID, side, TIF, price, or quantity");
}
inline void add_to_book(Book &book, const Order *order) {
    if (book.by_id.count(order->id.v))
        throw std::invalid_argument("duplicate active order ID");
    auto owned = std::make_unique<Order>(*order);
    auto *ptr = owned.get();
    auto &level = ptr->side == Side::Buy ? book.bids[ptr->px] : book.asks[ptr->px];
    level.q.push_back(ptr);
    level.total_qty += ptr->qty;
    book.positions.emplace(ptr->id.v, std::prev(level.q.end()));
    book.by_id.emplace(ptr->id.v, std::move(owned));
}
