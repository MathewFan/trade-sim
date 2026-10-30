#include "engine.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#define CHECK(condition) do {if(!(condition)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #condition);}while(false)
Order order(uint64_t id,Side side,long long px,long long qty,Tif tif=Tif::Day) {return {OrderId{id},"ABC",side,px,qty,1,tif};}
void priority_and_tif() {
    Book book;
    auto first=order(1,Side::Sell,100,5),second=order(2,Side::Sell,100,6),better=order(3,Side::Sell,99,2);
    match(book,first,1);match(book,second,2);match(book,better,3);
    auto buy=order(4,Side::Buy,100,9);const auto trades=match(book,buy,4);
    CHECK(trades.size()==3 && trades[0].resting_id.v==3 && trades[1].resting_id.v==1 && trades[2].resting_id.v==2);
    CHECK(trades[2].qty==2 && book.asks.at(100).total_qty==4 && book.by_id.size()==1 && valid_book(book));
    auto failed=order(5,Side::Buy,100,5,Tif::FOK);CHECK(match(book,failed,5).empty());CHECK(book.asks.at(100).total_qty==4);
    auto fok=order(6,Side::Buy,100,4,Tif::FOK);CHECK(match(book,fok,6).size()==1 && book.by_id.empty());
    auto ioc=order(7,Side::Buy,101,9,Tif::IOC);CHECK(match(book,ioc,7).empty() && book.by_id.empty());
    auto market=order(8,Side::Buy,0,9);CHECK(match(book,market,8).empty() && book.by_id.empty());
    auto cancel_order=order(9,Side::Buy,90,10);match(book,cancel_order,9);CHECK(cancel(book,9)&&!cancel(book,9)&&valid_book(book));
    auto invalid=order(10,Side::Buy,100,-1);try {match(book,invalid,10);CHECK(false);}catch(const std::invalid_argument&){}
}
void differential() {
    // Slow independent vector matcher, stable arrival sequence is the FIFO oracle.
    std::mt19937 rng(20261030);Book book;std::vector<Order> reference;
    for(uint64_t id=1;id<=10000;id++) {
        if(id%5==0 && !reference.empty()) {
            const auto index=rng()%reference.size();const auto target=reference[index].id.v;
            CHECK(cancel(book,target));reference.erase(reference.begin()+index);CHECK(valid_book(book));continue;
        }
        Order incoming=order(id,rng()%2?Side::Buy:Side::Sell,rng()%9==0?0:90+rng()%21,1+rng()%20,static_cast<Tif>(rng()%3));
        const auto original=incoming;
        std::vector<Trade> expected;
        auto crosses=[&](const Order& resting){return resting.side!=incoming.side && (incoming.px==0 || (incoming.side==Side::Buy?resting.px<=incoming.px:resting.px>=incoming.px));};
        long long available=0;for(const auto& value:reference) if(crosses(value)) available+=value.qty;
        if(incoming.tif!=Tif::FOK || available>=incoming.qty) {
            while(incoming.qty>0) {
                auto best=reference.end();
                for(auto it=reference.begin();it!=reference.end();++it) if(crosses(*it) && (best==reference.end() || (incoming.side==Side::Buy?it->px<best->px:it->px>best->px) || (it->px==best->px && it->id.v<best->id.v))) best=it;
                if(best==reference.end()) break;
                const auto q=std::min(incoming.qty,best->qty);expected.push_back({best->id,incoming.id,best->px,q,static_cast<long long>(id),1});incoming.qty-=q;best->qty-=q;
                if(!best->qty) reference.erase(best);
            }
            if(incoming.qty && incoming.tif==Tif::Day && incoming.px>0) reference.push_back(incoming);
        }
        auto actual_incoming=original;const auto actual=match(book,actual_incoming,id);
        CHECK(actual.size()==expected.size());
        for(size_t i=0;i<actual.size();i++) CHECK(actual[i].resting_id.v==expected[i].resting_id.v && actual[i].px==expected[i].px && actual[i].qty==expected[i].qty);
        CHECK(valid_book(book) && book.by_id.size()==reference.size());
        for(const auto& resting:reference) CHECK(book.by_id.at(resting.id.v)->qty==resting.qty);
    }
}
void concurrency_and_isolation() {
    Engine engine(1);engine.start();
    auto sell=engine.submit("AAA",Side::Sell,10,10,Tif::Day);sell.completion.get();
    auto buy=engine.submit("BBB",Side::Buy,20,10,Tif::Day);CHECK(buy.completion.get().trades.empty());
    CHECK(engine.snapshot("AAA").get().active_orders==1 && engine.snapshot("BBB").get().active_orders==1);
    CHECK(engine.cancel_async(sell.id).get().cancelled);CHECK(!engine.cancel_async(sell.id).get().cancelled);
    std::vector<std::thread> clients;
    for(int client=0;client<8;client++) clients.emplace_back([&,client]{for(int i=0;i<1000;i++) engine.submit_new("C"+std::to_string(client),Side::Buy,10,1,Tif::Day);});
    for(auto& client:clients) client.join();engine.flush();
    for(int client=0;client<8;client++) {const auto snapshot=engine.snapshot("C"+std::to_string(client)).get();CHECK(snapshot.active_orders==1000 && snapshot.bids[0].qty==1000);}
    engine.stop();try{engine.submit_new("X",Side::Buy,1,1,Tif::Day);CHECK(false);}catch(const std::logic_error&){}
    try{Engine bad(0);CHECK(false);}catch(const std::invalid_argument&){}
}
int main() {try{priority_and_tif();differential();concurrency_and_isolation();std::cout<<"PASS price/FIFO, TIF, 10,000 differential commands, 8 concurrent producers, symbol isolation\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
