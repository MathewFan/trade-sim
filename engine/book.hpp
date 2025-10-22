#pragma once
#include <map>
#include <deque>
#include <unordered_map>
#include <string>
#include <cstdint>

enum class Side : uint8_t { Buy=0, Sell=1 };
enum class Tif  : uint8_t { Day=0, IOC=1, FOK=2 };

struct OrderId { uint64_t v; };

struct Order {
    OrderId id; std::string sym; Side side;
    long long px; long long qty; long long ts_ns;
    Tif tif{Tif::Day};
};

struct Trade {
    OrderId resting_id, incoming_id;
    long long px, qty, seq, ts_ns;
};

struct PriceLevel { std::deque<Order*> q; long long total_qty = 0; };

using BidMap = std::map<long long, PriceLevel, std::greater<long long>>; // best-high-first
using AskMap = std::map<long long, PriceLevel, std::less<long long>>;    // best-low-first

struct Book {
    BidMap bids;
    AskMap asks;
    std::unordered_map<uint64_t, Order*> by_id;
};

inline void add_to_book(Book& b, Order* o){
    if (o->side==Side::Buy) { auto& lvl=b.bids[o->px]; lvl.q.push_back(o); lvl.total_qty+=o->qty; }
    else                    { auto& lvl=b.asks[o->px]; lvl.q.push_back(o); lvl.total_qty+=o->qty; }
    b.by_id[o->id.v]=o;
}
