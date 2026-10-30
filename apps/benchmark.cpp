#include "engine.hpp"
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <unistd.h>
using Clock = std::chrono::steady_clock;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Pending {
    Ticket ticket;
    Clock::time_point start;
};
void scenario(size_t shards, size_t clients, size_t count, const std::string &path) {
    Engine engine(shards, {path, true});
    engine.start();
    std::vector<double> latency;
    std::mutex mutex;
    size_t trades = 0;
    std::exception_ptr failure;
    const auto start = Clock::now();
    std::vector<std::thread> producers;
    for (size_t client = 0; client < clients; client++)
        producers.emplace_back([&, client] {
            try {
                std::vector<Pending> pending;
                std::vector<double> samples;
                size_t fills = 0;
                auto drain = [&] {
                    for (auto &item : pending) {
                        const auto report = item.ticket.completion.get();
                        fills += report.trades.size();
                        samples.push_back(
                            std::chrono::duration<double, std::micro>(Clock::now() - item.start)
                                .count());
                    }
                    pending.clear();
                };
                for (size_t i = client; i < count; i += clients) {
                    const auto begin = Clock::now();
                    auto ticket = engine.submit("S" + std::to_string((i / 2) % 64),
                                                i % 2 ? Side::Sell : Side::Buy, 100, 1, Tif::Day);
                    pending.push_back({std::move(ticket), begin});
                    if (pending.size() == 64)
                        drain();
                }
                drain();
                std::lock_guard<std::mutex> lock(mutex);
                trades += fills;
                latency.insert(latency.end(), samples.begin(), samples.end());
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!failure)
                    failure = std::current_exception();
            }
        });
    for (auto &producer : producers)
        producer.join();
    engine.flush();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    if (failure)
        std::rethrow_exception(failure);
    require(trades == count / 2, "trade conservation failed");
    require(latency.size() == count, "completion count mismatch");
    for (size_t i = 0; i < 64; i++)
        require(engine.snapshot("S" + std::to_string(i)).get().active_orders == 0,
                "unmatched benchmark quantity");
    engine.stop();
    std::sort(latency.begin(), latency.end());
    std::cout << "{\"mode\":\"" << (path.empty() ? "memory" : "wal-fsync")
              << "\",\"shards\":" << shards << ",\"producers\":" << clients
              << ",\"commands\":" << count << ",\"trades\":" << trades << ",\"seconds\":" << elapsed
              << ",\"commands_per_second\":" << count / elapsed
              << ",\"observed_completion_us\":{\"p50\":" << latency[latency.size() / 2]
              << ",\"p95\":" << latency[latency.size() * 95 / 100]
              << ",\"p99\":" << latency[latency.size() * 99 / 100] << "}";
    // Close the engine (and release WAL locks) before reopening for recovery below.
    std::cout << "}";
}
void replay_trial(const std::string &path) {
    const auto start = Clock::now();
    Engine engine(1, {path, true});
    engine.start();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    for (size_t i = 0; i < 64; i++)
        require(engine.snapshot("S" + std::to_string(i)).get().active_orders == 0,
                "recovery end-state mismatch");
    engine.stop();
    std::cout << ",\"wal_recovery_ms\":" << elapsed;
}
void cancellation(size_t count) {
    Book book;
    std::vector<uint64_t> ids(count);
    std::iota(ids.begin(), ids.end(), 1);
    for (auto id : ids) {
        Order order{OrderId{id}, "CANCEL", Side::Buy, 100, 1, 1, Tif::Day};
        match(book, order, id);
    }
    std::mt19937 rng(42);
    std::shuffle(ids.begin(), ids.end(), rng);
    const auto start = Clock::now();
    for (auto id : ids)
        require(cancel(book, id), "cancel failed");
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    require(valid_book(book) && book.by_id.empty(), "cancel end-state mismatch");
    std::cout << ",\"same_level_cancel\":{\"orders\":" << count << ",\"seconds\":" << elapsed
              << ",\"cancels_per_second\":" << count / elapsed << "}";
}
} // namespace
int main(int argc, char **argv) {
    std::filesystem::path directory;
    try {
        const auto requested = argc > 1 ? std::stoull(argv[1]) : 100000;
        if (requested < 128 || requested > 1000000)
            throw std::invalid_argument("commands must be 128..1000000");
        const auto count = static_cast<size_t>(requested / 128 * 128);
        directory = std::filesystem::temp_directory_path() /
                    ("tradesim-bench-" + std::to_string(::getpid()) + "-" +
                     std::to_string(std::random_device{}()));
        std::cout
            << std::setprecision(9) << "{\"compiler\":\"" << __VERSION__
            << "\",\"hardware_threads\":" << std::thread::hardware_concurrency()
            << ",\"seed\":42,\"latency_scope\":\"submit to observed completion; 64 outstanding per "
               "producer, includes head-of-line observation delay\",\"scenarios\":[";
        scenario(1, 1, count, "");
        std::cout << ",";
        scenario(1, 4, count, "");
        std::cout << ",";
        scenario(4, 4, count, "");
        std::cout << ",";
        scenario(1, 1, 1024, directory.string());
        std::cout << "]";
        replay_trial(directory.string());
        cancellation(count);
        std::cout << ",\"verified\":true}\n";
        std::filesystem::remove_all(directory);
    } catch (const std::exception &error) {
        if (!directory.empty())
            std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
