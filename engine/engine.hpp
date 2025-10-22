#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <cstdint>
#include <array>
#include <functional>
#include "ring.hpp"
#include "book.hpp"
#include "match.hpp"

enum class MsgType : uint8_t { New=0, Cancel=1, Print=2, Stop=3 };

struct Msg {
    MsgType type{MsgType::New};
    Order order{};             // valid if type==New
    uint64_t cancel_id{0};     // valid if type==Cancel
    std::string print_sym;     // valid if type==Print
};

class WalWriter; // fwd-declare

struct ShardWorker {
    using Ring = SpscRing<Msg, 1 << 16>;
    Ring ring;
    std::thread th;
    std::atomic<bool> running{false};
    std::vector<std::unique_ptr<Order>> arena;
    Book book;
    std::atomic<long long> seq{0};
    std::unique_ptr<WalWriter> wal;
    std::array<uint8_t,32> state_root{};
};

struct Engine {
    explicit Engine(size_t n_shards);
    ~Engine();

    void start();
    void stop();

    // API
    uint64_t submit_new(const std::string& sym, Side side, long long px, long long qty, Tif tif);
    void     submit_cancel(uint64_t id);
    void     submit_print(const std::string& sym);

    // Event sink (optional): called on trades and on state-root prints
    void set_event_sink(std::function<void(const std::string&)> cb){ on_event = std::move(cb); }

private:
    size_t nshards;
    std::vector<std::unique_ptr<ShardWorker>> shards;
    std::atomic<uint64_t> next_id{1};
    std::mutex print_mtx; // for stdout serialization
    std::function<void(const std::string&)> on_event; // JSON lines to broadcast

    size_t shard_of(const std::string& sym) const {
        return std::hash<std::string>{}(sym) % nshards;
    }

    // --- serialization for WAL "raw_msg" ---
    static std::vector<uint8_t> ser_new(const Order& o){
        std::string s = "N " + o.sym + " " + (o.side==Side::Buy?"B":"S") + " " +
                        std::to_string(o.px) + " " + std::to_string(o.qty) + " " +
                        (o.tif==Tif::IOC?"IOC":(o.tif==Tif::FOK?"FOK":"DAY"));
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    static std::vector<uint8_t> ser_cancel(uint64_t id){
        std::string s = "C " + std::to_string(id);
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    static std::vector<uint8_t> ser_print(const std::string& sym){
        std::string s = "P " + sym;
        return std::vector<uint8_t>(s.begin(), s.end());
    }
};
