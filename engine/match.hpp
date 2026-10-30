#pragma once
#include "book.hpp"
#include <algorithm>
#include <limits>

template<typename MapT,typename Cross>
inline void cross_and_fill(Book& book,MapT& contra,Order& incoming,long long seq,Cross cross,std::vector<Trade>& out) {
    for(auto level=contra.begin();level!=contra.end() && incoming.qty>0 && cross(level->first);) {
        auto& value=level->second;
        while(!value.q.empty() && incoming.qty>0) {
            auto* resting=value.q.front();
            const auto quantity=std::min(resting->qty,incoming.qty);
            out.push_back({resting->id,incoming.id,level->first,quantity,seq,incoming.ts_ns});
            resting->qty-=quantity; incoming.qty-=quantity; value.total_qty-=quantity;
            if(!resting->qty) {
                const auto id=resting->id.v; value.q.pop_front();
                book.positions.erase(id); book.by_id.erase(id);
            }
        }
        if(value.q.empty()) level=contra.erase(level); else ++level;
    }
}
inline std::vector<Trade> match(Book& book,Order& incoming,long long seq) {
    validate_order(incoming);
    if(book.by_id.count(incoming.id.v)) throw std::invalid_argument("duplicate active order ID");
    // Keep aggregate quantities representable and active storage explicitly bounded.
    if(book.by_id.size()>=1000000) throw std::length_error("active-order capacity exceeded");
    std::vector<Trade> trades;
    if(incoming.tif==Tif::FOK) {
        auto need=incoming.qty;
        auto sufficient=[&](const auto& levels,auto crosses) {
            for(const auto& [price,level]:levels) {
                if(!crosses(price)) break;
                if(level.total_qty>=need) return true;
                need-=level.total_qty;
            }
            return false;
        };
        const bool fillable=incoming.side==Side::Buy?
            sufficient(book.asks,[&](auto px){return incoming.px==0||px<=incoming.px;}):
            sufficient(book.bids,[&](auto px){return incoming.px==0||px>=incoming.px;});
        if(!fillable) return trades; // No mutations on failed FOK.
    }
    if(incoming.side==Side::Buy)
        cross_and_fill(book,book.asks,incoming,seq,[&](auto px){return incoming.px==0||px<=incoming.px;},trades);
    else cross_and_fill(book,book.bids,incoming,seq,[&](auto px){return incoming.px==0||px>=incoming.px;},trades);
    if(incoming.qty>0 && incoming.tif==Tif::Day && incoming.px>0) add_to_book(book,&incoming);
    return trades;
}
inline bool cancel(Book& book,uint64_t id) {
    auto found=book.by_id.find(id);
    if(found==book.by_id.end()) return false;
    const auto* order=found->second.get();
    auto remove=[&](auto& levels) {
        auto level=levels.find(order->px);
        level->second.total_qty-=order->qty;
        level->second.q.erase(book.positions.at(id));
        if(level->second.q.empty()) levels.erase(level);
    };
    if(order->side==Side::Buy) remove(book.bids); else remove(book.asks);
    book.positions.erase(id); book.by_id.erase(found); return true;
}
inline bool valid_book(const Book& book) {
    size_t count=0;
    auto check=[&](const auto& levels,Side side) {
        for(const auto& [price,level]:levels) {
            if(level.q.empty()) return false;
            long long total=0;
            for(auto iter=level.q.begin();iter!=level.q.end();++iter) {
                const auto* order=*iter;
                const auto found=book.by_id.find(order->id.v);
                const auto position=book.positions.find(order->id.v);
                if(order->qty<=0 || order->px!=price || order->side!=side || found==book.by_id.end() ||
                   found->second.get()!=order || position==book.positions.end() || *position->second!=order) return false;
                total+=order->qty; ++count;
            }
            if(total!=level.total_qty) return false;
        }
        return true;
    };
    return check(book.bids,Side::Buy) && check(book.asks,Side::Sell) &&
        count==book.by_id.size() && count==book.positions.size() &&
        (book.bids.empty() || book.asks.empty() || book.bids.begin()->first<book.asks.begin()->first);
}
