#pragma once
#include "match.hpp"
#include "ring.hpp"
#include "wal.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>

struct DepthLevel {
    long long px, qty;
    size_t orders;
};
struct Snapshot {
    std::string sym;
    uint64_t sequence{};
    size_t active_orders{};
    std::vector<DepthLevel> bids, asks;
    std::string canonical, state_hash, journal_tip;
};
struct ExecutionReport {
    uint64_t id{}, sequence{};
    std::vector<Trade> trades;
    long long remaining{};
    bool resting{}, cancelled{};
};
struct Ticket {
    uint64_t id;
    std::future<ExecutionReport> completion;
};
enum class MsgType : uint8_t { New, Cancel, Print, Snapshot, Barrier };
struct Msg {
    MsgType type{MsgType::New};
    Order order{};
    uint64_t cancel_id{};
    std::string print_sym;
    std::shared_ptr<std::promise<ExecutionReport>> report;
    std::shared_ptr<std::promise<::Snapshot>> snapshot;
    std::shared_ptr<std::promise<void>> barrier;
};
struct ShardWorker {
    SpscRing<Msg, 4096> ring;
    std::timed_mutex producer_mutex;
    std::thread thread;
    std::atomic<bool> running{false};
    std::map<std::string, Book> books;
    std::unordered_map<uint64_t, std::string> active_symbols;
    uint64_t sequence{};
    std::unique_ptr<WalWriter> wal;
    std::array<uint8_t, 32> journal_tip{};
};
struct EngineOptions {
    std::string wal_directory;
    bool sync_wal{true};
};
class Engine {
  public:
    explicit Engine(size_t shards, EngineOptions options = {});
    ~Engine();
    void start();
    void stop();
    void flush();
    Ticket submit(const std::string &sym, Side side, long long px, long long qty, Tif tif);
    uint64_t submit_new(const std::string &sym, Side side, long long px, long long qty, Tif tif) {
        return submit(sym, side, px, qty, tif).id;
    }
    std::future<ExecutionReport> cancel_async(uint64_t id);
    void submit_cancel(uint64_t id) {
        (void)cancel_async(id);
    }
    void submit_print(const std::string &sym);
    std::future<Snapshot> snapshot(const std::string &sym);
    void set_event_sink(std::function<void(const std::string &)> sink);
    static uint64_t symbol_hash(const std::string &sym) {
        uint64_t value = 14695981039346656037ULL;
        for (unsigned char byte : sym) {
            value ^= byte;
            value *= 1099511628211ULL;
        }
        return value;
    }

  private:
    size_t nshards;
    EngineOptions options;
    std::vector<std::unique_ptr<ShardWorker>> shards;
    std::atomic<uint64_t> next_id{1};
    std::shared_mutex lifecycle;
    bool started{}, accepting{};
    std::mutex failure_mutex, sink_mutex;
    std::exception_ptr failure;
    std::function<void(const std::string &)> on_event;
    void enqueue(size_t shard, Msg message);
    void worker(ShardWorker &shard);
    void apply(ShardWorker &shard, Msg &message, bool recovering = false);
    void check_failure();
    void emit(const std::string &event);
    Snapshot make_snapshot(const ShardWorker &shard, const std::string &sym) const;
    size_t shard_of(const std::string &sym) const {
        return symbol_hash(sym) % nshards;
    }
};
