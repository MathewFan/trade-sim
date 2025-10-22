#include <bits/stdc++.h>
using namespace std;

enum class Side: uint8_t { Buy, Sell };
enum class Tif:  uint8_t { Day, IOC, FOK };

struct OrderId { uint64_t v; };
struct Order {
  OrderId id; string sym; Side side;
  long long px; long long qty; long long ts_ns;
  Tif tif{Tif::Day};
};
struct Trade {
  OrderId resting_id, incoming_id;
  long long px, qty, seq, ts_ns;
};
struct PriceLevel { deque<Order*> q; long long total_qty=0; };

using BidMap = map<long long, PriceLevel, greater<long long>>; // best-first: highest price
using AskMap = map<long long, PriceLevel, less<long long>>;    // best-first: lowest price

struct Book {
  BidMap bids;
  AskMap asks;
  unordered_map<uint64_t,Order*> by_id;
};

static inline void add_to_book(Book& b, Order* o){
  if (o->side==Side::Buy) {
    auto& lvl = b.bids[o->px];
    lvl.q.push_back(o); lvl.total_qty += o->qty;
  } else {
    auto& lvl = b.asks[o->px];
    lvl.q.push_back(o); lvl.total_qty += o->qty;
  }
  b.by_id[o->id.v] = o;
}

// common inner loop
template<typename MapT, typename CrossFn>
static inline void cross_and_fill(Book& b, MapT& contra, Order& in, long long seq, CrossFn cross, vector<Trade>& out){
  for (auto it = contra.begin(); it!=contra.end() && in.qty>0 && cross(it->first); ){
    auto& lvl = it->second;
    while(!lvl.q.empty() && in.qty>0){
      Order* r = lvl.q.front();
      long long q = min(in.qty, r->qty);
      r->qty -= q; in.qty -= q; lvl.total_qty -= q;
      out.push_back(Trade{r->id, in.id, it->first, q, seq, in.ts_ns});
      if (r->qty==0){ b.by_id.erase(r->id.v); lvl.q.pop_front(); }
    }
    if (lvl.q.empty()) it = contra.erase(it); else ++it;
  }
}

static inline vector<Trade> match(Book& b, Order& in, long long seq){
  vector<Trade> out;
  if (in.side==Side::Buy){
    auto cross = [&](long long px){ return (px<=in.px || in.px==0); };
    cross_and_fill(b, b.asks, in, seq, cross, out);
  } else {
    auto cross = [&](long long px){ return (px>=in.px || in.px==0); };
    cross_and_fill(b, b.bids, in, seq, cross, out);
  }
  if (in.qty>0 && in.tif==Tif::Day && in.px>0) add_to_book(b,&in);
  return out;
}

int main(){
  ios::sync_with_stdio(false); cin.tie(nullptr);
  Book book; vector<unique_ptr<Order>> arena;
  uint64_t next_id=1; long long seq=0; string cmd;

  while (cin>>cmd){
    if (cmd=="N"){
      string sym, side_str, tif_str="DAY"; long long px, qty;
      cin>>sym>>side_str>>px>>qty>>tif_str;
      Side side = (side_str=="B") ? Side::Buy : Side::Sell;
      Tif tif = (tif_str=="IOC")?Tif::IOC : (tif_str=="FOK")?Tif::FOK : Tif::Day;

      arena.emplace_back(make_unique<Order>(
        Order{OrderId{next_id++}, sym, side, px, qty,
              chrono::high_resolution_clock::now().time_since_epoch().count(),
              tif}
      ));
      auto& o = *arena.back();
      auto trades = match(book, o, ++seq);
      for (auto& t: trades)
        cout << "T " << t.seq << " " << o.sym << " " << t.px << " " << t.qty << "\n";

    } else if (cmd=="C"){
      uint64_t id; cin>>id;
      auto it = book.by_id.find(id);
      if (it!=book.by_id.end()){
        Order* o = it->second;
        if (o->side==Side::Buy){
          auto lit = book.bids.find(o->px);
          if (lit!=book.bids.end()){
            auto& q = lit->second.q;
            for (auto qi=q.begin(); qi!=q.end(); ++qi){
              if((*qi)->id.v==id){ lit->second.total_qty-=(*qi)->qty; q.erase(qi); break; }
            }
            if (lit->second.q.empty()) book.bids.erase(lit);
          }
        } else {
          auto lit = book.asks.find(o->px);
          if (lit!=book.asks.end()){
            auto& q = lit->second.q;
            for (auto qi=q.begin(); qi!=q.end(); ++qi){
              if((*qi)->id.v==id){ lit->second.total_qty-=(*qi)->qty; q.erase(qi); break; }
            }
            if (lit->second.q.empty()) book.asks.erase(lit);
          }
        }
        book.by_id.erase(it);
        cout << "A CANCELLED " << id << "\n";
      }
    } else if (cmd=="P"){
      string sym; cin>>sym; // (unused in MVP)
      auto print_side = [&](auto& side){
        int shown=0;
        for (auto& [px, lvl] : side){
          if(shown++==5) break;
          long long agg=0; for (auto* o : lvl.q) agg += o->qty;
          cout << px << ":" << agg << " ";
        }
        cout << "\n";
      };
      cout << "BIDS "; print_side(book.bids);
      cout << "ASKS "; print_side(book.asks);
    }
  }
}
