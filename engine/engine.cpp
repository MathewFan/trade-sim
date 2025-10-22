#include "engine.hpp"
#include "wal.hpp"
#include <iostream>
#include <chrono>

using std::cout;
using std::string;

Engine::Engine(size_t n_shards) : nshards(n_shards) {
    shards.reserve(n_shards);
    for (size_t i=0;i<n_shards;++i){
        shards.emplace_back(std::make_unique<ShardWorker>());
    }
}

Engine::~Engine(){ stop(); }

void Engine::start(){
    for (size_t i=0; i<shards.size(); ++i){
        auto& s = shards[i];
        s->running.store(true, std::memory_order_relaxed);
        s->wal = std::make_unique<WalWriter>("wal_shard" + std::to_string(i) + ".bin");
        s->state_root = s->wal->prev_hash;

        s->th = std::thread([this, &s](){
            Msg msg;
            while (s->running.load(std::memory_order_relaxed) || !s->ring.empty()){
                if (!s->ring.pop(msg)){
                    std::this_thread::yield();
                    continue;
                }
                switch (msg.type){
                    case MsgType::New: {
                        s->arena.emplace_back(std::make_unique<Order>(msg.order));
                        Order& in = *s->arena.back();
                        auto trades = match(s->book, in, s->seq.fetch_add(1, std::memory_order_relaxed)+1);

                        // WAL append
                        auto raw = ser_new(in);
                        s->wal->append_record(RecType::New,
                                              (uint64_t)s->seq.load(std::memory_order_relaxed),
                                              (uint64_t)in.ts_ns,
                                              raw, trades);
                        s->state_root = s->wal->prev_hash;

                        // stdout + event sink
                        if (!trades.empty()){
                            std::lock_guard<std::mutex> lk(print_mtx);
                            for (auto& t : trades){
                                cout << "T " << t.seq << " " << in.sym << " " << t.px << " " << t.qty << "\n";
                                if (on_event){
                                    // {"event":"trade","seq":...,"sym":"ABC","px":100,"qty":5}
                                    on_event(std::string("{\"event\":\"trade\",\"seq\":") +
                                             std::to_string(t.seq) + ",\"sym\":\"" + in.sym +
                                             "\",\"px\":" + std::to_string(t.px) +
                                             ",\"qty\":" + std::to_string(t.qty) + "}");
                                }
                            }
                        }
                        break;
                    }
                    case MsgType::Cancel: {
                        bool ok = cancel(s->book, msg.cancel_id);
                        auto raw = ser_cancel(msg.cancel_id);
                        std::vector<Trade> none;
                        s->wal->append_record(RecType::Cancel,
                                              (uint64_t)s->seq.load(std::memory_order_relaxed),
                                              (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count(),
                                              raw, none);
                        s->state_root = s->wal->prev_hash;

                        std::lock_guard<std::mutex> lk(print_mtx);
                        if (ok) cout << "A CANCELLED " << msg.cancel_id << "\n";
                        else    cout << "A UNKNOWN "    << msg.cancel_id << "\n";
                        break;
                    }
                    case MsgType::Print: {
                        auto raw = ser_print(msg.print_sym);
                        std::vector<Trade> none;
                        s->wal->append_record(RecType::Print,
                                              (uint64_t)s->seq.load(std::memory_order_relaxed),
                                              (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count(),
                                              raw, none);
                        s->state_root = s->wal->prev_hash;

                        std::lock_guard<std::mutex> lk(print_mtx);
                        auto dump = [](auto& side){
                            int shown=0;
                            for (auto& [px,lvl] : side){
                                if (shown++==5) break;
                                long long agg=0; for (auto* o : lvl.q) agg+=o->qty;
                                cout << px << ":" << agg << " ";
                            }
                            cout << "\n";
                        };
                        cout << "BIDS "; dump(s->book.bids);
                        cout << "ASKS "; dump(s->book.asks);
                        string root_hex = hex32(s->state_root);
                        cout << "STATE_ROOT " << root_hex << "\n";
                        if (on_event){
                            on_event(std::string("{\"event\":\"state_root\",\"sym\":\"") + msg.print_sym +
                                     "\",\"root\":\"" + root_hex + "\"}");
                        }
                        break;
                    }
                    case MsgType::Stop:
                    default: break;
                }
            }
        });
    }
}

void Engine::stop(){
    for (auto& s : shards){
        if (!s->running.load()) continue;
        s->running.store(false);
    }
    for (auto& s : shards){
        if (s->th.joinable()) s->th.join();
    }
}

uint64_t Engine::submit_new(const string& sym, Side side, long long px, long long qty, Tif tif){
    uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    Msg m;
    m.type = MsgType::New;
    m.order = Order{OrderId{id}, sym, side, px, qty,
                    std::chrono::high_resolution_clock::now().time_since_epoch().count(), tif};
    size_t i = shard_of(sym);
    while (!shards[i]->ring.push(m)) std::this_thread::yield();
    return id;
}

void Engine::submit_cancel(uint64_t id){
    Msg m; m.type = MsgType::Cancel; m.cancel_id = id;
    for (auto& s : shards){
        while (!s->ring.push(m)) std::this_thread::yield();
    }
}

void Engine::submit_print(const string& sym){
    size_t i = shard_of(sym);
    Msg m; m.type = MsgType::Print; m.print_sym = sym;
    while (!shards[i]->ring.push(m)) std::this_thread::yield();
}
