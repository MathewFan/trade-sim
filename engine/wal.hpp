#pragma once
#include "book.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Version 2: immutable input commands, never mutated residual quantities.
enum class RecType : uint8_t { New = 1, Cancel = 2 };
struct WalRecord {
    RecType type{RecType::New};
    uint64_t seq{}, ts_ns{};
    Order order{};
    uint64_t cancel_id{};
};
namespace wal_codec {
using Bytes = std::vector<uint8_t>;
inline void u32(Bytes &out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
inline void u64(Bytes &out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
struct Reader {
    const Bytes &bytes;
    size_t offset{};
    uint64_t integer(size_t length) {
        if (length > bytes.size() - offset)
            throw std::runtime_error("truncated WAL payload");
        uint64_t value = 0;
        for (size_t i = 0; i < length; i++)
            value = (value << 8) | bytes[offset++];
        return value;
    }
    long long signed_positive() {
        const auto value = integer(8);
        if (value > static_cast<uint64_t>(std::numeric_limits<long long>::max()))
            throw std::runtime_error("WAL integer outside signed range");
        return static_cast<long long>(value);
    }
};
inline Bytes header(uint32_t shards, uint32_t index) {
    const std::string magic = "TSWAL002";
    Bytes out(magic.begin(), magic.end());
    u32(out, shards);
    u32(out, index);
    return out;
}
inline Bytes encode(const WalRecord &record) {
    Bytes out;
    out.push_back(static_cast<uint8_t>(record.type));
    u64(out, record.seq);
    u64(out, record.ts_ns);
    if (record.type == RecType::New) {
        const auto &order = record.order;
        validate_order(order);
        u64(out, order.id.v);
        out.push_back(static_cast<uint8_t>(order.sym.size()));
        out.insert(out.end(), order.sym.begin(), order.sym.end());
        out.push_back(static_cast<uint8_t>(order.side));
        u64(out, order.px);
        u64(out, order.qty);
        out.push_back(static_cast<uint8_t>(order.tif));
    } else if (record.type == RecType::Cancel && record.cancel_id)
        u64(out, record.cancel_id);
    else
        throw std::invalid_argument("invalid WAL record type or cancel ID");
    return out;
}
inline WalRecord decode(const Bytes &bytes) {
    Reader input{bytes};
    WalRecord result;
    result.type = static_cast<RecType>(input.integer(1));
    result.seq = input.integer(8);
    result.ts_ns = input.integer(8);
    if (result.ts_ns > static_cast<uint64_t>(std::numeric_limits<long long>::max()))
        throw std::runtime_error("invalid WAL timestamp");
    if (result.type == RecType::New) {
        auto &order = result.order;
        order.id.v = input.integer(8);
        const auto size = input.integer(1);
        if (!size || size > 16 || size > bytes.size() - input.offset)
            throw std::runtime_error("invalid WAL symbol length");
        order.sym.assign(bytes.begin() + input.offset, bytes.begin() + input.offset + size);
        input.offset += size;
        order.side = static_cast<Side>(input.integer(1));
        order.px = input.signed_positive();
        order.qty = input.signed_positive();
        order.tif = static_cast<Tif>(input.integer(1));
        order.ts_ns = static_cast<long long>(result.ts_ns);
        validate_order(order);
    } else if (result.type == RecType::Cancel) {
        result.cancel_id = input.integer(8);
        if (!result.cancel_id)
            throw std::runtime_error("zero WAL cancel ID");
    } else
        throw std::runtime_error("unknown WAL record type");
    if (input.offset != bytes.size())
        throw std::runtime_error("unexpected WAL trailing payload");
    return result;
}
inline std::array<uint8_t, 32> chain(const std::array<uint8_t, 32> &previous, const Bytes &frame) {
    SHA256 hash;
    hash.update(previous.data(), previous.size());
    hash.update(frame);
    return hash.digest();
}
inline Bytes read_exact(std::ifstream &input, size_t size) {
    Bytes out(size);
    if (!input.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("truncated WAL: no automatic tail repair");
    return out;
}
} // namespace wal_codec
struct WalSummary {
    uint32_t shards{}, index{};
    uint64_t records{};
    std::array<uint8_t, 32> tip{};
};
inline WalSummary scan_wal(const std::string &path,
                           const std::function<void(const WalRecord &)> &visit = {}) {
    using namespace wal_codec;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open WAL " + path);
    const auto head = read_exact(input, 16);
    if (std::string(head.begin(), head.begin() + 8) != "TSWAL002")
        throw std::runtime_error("unsupported WAL version (legacy logs are not replayable)");
    Reader reader{head, 8};
    WalSummary summary;
    summary.shards = static_cast<uint32_t>(reader.integer(4));
    summary.index = static_cast<uint32_t>(reader.integer(4));
    if (!summary.shards || summary.shards > 64 || summary.index >= summary.shards)
        throw std::runtime_error("invalid WAL shard topology");
    summary.tip = SHA256::hash(head);
    while (input.peek() != std::char_traits<char>::eof()) {
        auto frame = read_exact(input, 4);
        Reader length_reader{frame};
        const auto length = length_reader.integer(4);
        if (length < 25 || length > 128)
            throw std::runtime_error("WAL frame length outside 25..128");
        const auto body = read_exact(input, static_cast<size_t>(length));
        frame.insert(frame.end(), body.begin(), body.end());
        const auto post = read_exact(input, 32);
        const auto expected = chain(summary.tip, frame);
        if (!std::equal(expected.begin(), expected.end(), post.begin()))
            throw std::runtime_error("WAL hash mismatch at record " +
                                     std::to_string(summary.records + 1));
        const auto record = decode(body);
        if (record.seq != summary.records + 1)
            throw std::runtime_error("non-contiguous WAL sequence");
        ++summary.records;
        summary.tip = expected;
        if (visit)
            visit(record);
    }
    if (input.bad())
        throw std::runtime_error("WAL read failed");
    return summary;
}
class WalWriter {
    int fd{-1};
    bool sync;
    void write_all(const wal_codec::Bytes &bytes) {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("WAL write failed");
            offset += static_cast<size_t>(n);
        }
    }
    void synchronize() {
        if (sync && ::fsync(fd) != 0)
            throw std::runtime_error("WAL fsync failed");
    }

  public:
    std::string path;
    std::array<uint8_t, 32> prev_hash{};
    uint64_t sequence{};
    WalWriter(const std::string &file, uint32_t count, uint32_t index, bool durable)
        : sync(durable), path(file) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        const bool created = fd >= 0;
        if (fd < 0 && errno == EEXIST)
            fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0)
            throw std::runtime_error("cannot create/open WAL " + path);
        try {
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("WAL already has a writer");
            struct stat info{};
            if (::fstat(fd, &info) != 0)
                throw std::runtime_error("cannot stat WAL");
            if (info.st_size == 0 && !created)
                throw std::runtime_error(
                    "existing empty WAL: refusing to erase possible truncation evidence");
            if (created) {
                const auto head = wal_codec::header(count, index);
                write_all(head);
                synchronize();
                // Persist newly-created directory entries as well as file contents.
                if (sync) {
                    const auto parent = std::filesystem::path(path).parent_path();
                    const int dir =
                        ::open((parent.empty() ? "." : parent.string()).c_str(), O_RDONLY);
                    if (dir < 0)
                        throw std::runtime_error("cannot open WAL directory");
                    const int rc = ::fsync(dir);
                    ::close(dir);
                    if (rc != 0)
                        throw std::runtime_error("WAL directory fsync failed");
                }
            }
            const auto summary = scan_wal(path);
            if (summary.shards != count || summary.index != index)
                throw std::runtime_error("WAL shard topology mismatch");
            prev_hash = summary.tip;
            sequence = summary.records;
            if (::lseek(fd, 0, SEEK_END) < 0)
                throw std::runtime_error("cannot seek WAL");
        } catch (...) {
            ::close(fd);
            fd = -1;
            throw;
        }
    }
    WalWriter(const WalWriter &) = delete;
    WalWriter &operator=(const WalWriter &) = delete;
    ~WalWriter() {
        if (fd >= 0)
            ::close(fd);
    }
    void append(const WalRecord &record) {
        if (record.seq != sequence + 1)
            throw std::runtime_error("WAL append sequence mismatch");
        const auto body = wal_codec::encode(record);
        wal_codec::Bytes frame;
        wal_codec::u32(frame, static_cast<uint32_t>(body.size()));
        frame.insert(frame.end(), body.begin(), body.end());
        const auto hash = wal_codec::chain(prev_hash, frame);
        frame.insert(frame.end(), hash.begin(), hash.end());
        write_all(frame);
        synchronize();
        prev_hash = hash;
        sequence = record.seq;
    }
};
