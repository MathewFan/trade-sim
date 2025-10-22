#pragma once
#include "book.hpp"
#include <vector>

template<typename MapT, typename CrossFn>
inline void cross_and_fill(Book& b, MapT& contra, Order& in, long long seq, CrossFn cross, std::vector<Trade>& out){
    for (auto it=contra.begin(); it!=contra.end() && in.qty>0 && cross(it->first); ){
        auto& lvl = it->second;
        while(!lvl.q.empty() && in.qty>0){
            Order* r = lvl.q.front();
            long long q = std::min(in.qty, r->qty);
            r->qty -= q; in.qty -= q; lvl.total_qty -= q;
            out.push_back(Trade{r->id, in.id, it->first, q, seq, in.ts_ns});
            if (r->qty==0){ b.by_id.erase(r->id.v); lvl.q.pop_front(); }
        }
        if (lvl.q.empty()) it = contra.erase(it); else ++it;
    }
}

inline std::vector<Trade> match(Book& b, Order& in, long long seq){
    std::vector<Trade> out;
    if (in.tif==Tif::FOK){
        // Dry-run check for total available qty at acceptable prices
        long long need = in.qty, have = 0;
        if (in.side==Side::Buy){
            for (auto& [px, lvl] : b.asks){ if (px>in.px && in.px!=0) break; for (auto* o: lvl.q) have += o->qty; if (have>=need) break; }
        } else {
            for (auto& [px, lvl] : b.bids){ if (px<in.px && in.px!=0) break; for (auto* o: lvl.q) have += o->qty; if (have>=need) break; }
        }
        if (have < need) return out; // cancel entirely
    }

    if (in.side==Side::Buy){
        auto cross = [&](long long px){ return (px<=in.px || in.px==0); };
        cross_and_fill(b, b.asks, in, seq, cross, out);
    } else {
        auto cross = [&](long long px){ return (px>=in.px || in.px==0); };
        cross_and_fill(b, b.bids, in, seq, cross, out);
    }

    // Place residual if not IOC/FOK and it's a limit
    if (in.qty>0 && in.tif==Tif::Day && in.px>0) add_to_book(b,&in);
    return out;
}

// Cancel by ID (returns true if removed)
inline bool cancel(Book& b, uint64_t id){
    auto it = b.by_id.find(id);
    if (it==b.by_id.end()) return false;
    Order* o = it->second;
    if (o->side==Side::Buy){
        auto lit = b.bids.find(o->px);
        if (lit!=b.bids.end()){
            auto& q = lit->second.q;
            for (auto qi=q.begin(); qi!=q.end(); ++qi){
                if ((*qi)->id.v==id){ lit->second.total_qty -= (*qi)->qty; q.erase(qi); break; }
            }
            if (lit->second.q.empty()) b.bids.erase(lit);
        }
    } else {
        auto lit = b.asks.find(o->px);
        if (lit!=b.asks.end()){
            auto& q = lit->second.q;
            for (auto qi=q.begin(); qi!=q.end(); ++qi){
                if ((*qi)->id.v==id){ lit->second.total_qty -= (*qi)->qty; q.erase(qi); break; }
            }
            if (lit->second.q.empty()) b.asks.erase(lit);
        }
    }
    b.by_id.erase(it);
    return true;
}
