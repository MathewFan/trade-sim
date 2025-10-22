#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include "sha256.hpp"
#include "book.hpp"

enum class RecType : uint8_t { New=1, Cancel=2, Print=3 };

struct WalWriter {
    std::string path;
    std::FILE*  fp{nullptr};
    std::array<uint8_t,32> prev_hash{}; // starts at zero
    std::mutex mtx; // serialize writes

    explicit WalWriter(const std::string& p): path(p){
        fp = std::fopen(path.c_str(), "ab");
    }
    ~WalWriter(){ if(fp) std::fclose(fp); }

    inline void write_u8(uint8_t v){ std::fwrite(&v,1,1,fp); }
    inline void write_u32(uint32_t v){ std::fwrite(&v,4,1,fp); }
    inline void write_u64(uint64_t v){ std::fwrite(&v,8,1,fp); }
    inline void write_bytes(const void* p, size_t n){ std::fwrite(p,1,n,fp); }

    // serialize record into a buffer (excluding post_hash), compute hash chaining, then append hash and flush buffer.
    void append_record(RecType typ, uint64_t seq, uint64_t ts_ns,
                       const std::vector<uint8_t>& raw_msg,
                       const std::vector<Trade>& trades){
        std::lock_guard<std::mutex> lk(mtx);
        // build the byte buffer for hashing + writing
        std::vector<uint8_t> buf;
        buf.reserve(1+8+8+4+raw_msg.size()+4 + trades.size()* (8+8+8+8));

        auto put_u8=[&](uint8_t v){ buf.push_back(v); };
        auto put_u32=[&](uint32_t v){ for(int i=3;i>=0;--i) buf.push_back( (v>>(8*i))&0xFF ); };
        auto put_u64=[&](uint64_t v){ for(int i=7;i>=0;--i) buf.push_back( (v>>(8*i))&0xFF ); };

        put_u8((uint8_t)typ);
        put_u64(seq);
        put_u64(ts_ns);
        put_u32((uint32_t)raw_msg.size());
        buf.insert(buf.end(), raw_msg.begin(), raw_msg.end());
        put_u32((uint32_t)trades.size());
        for (auto& t : trades){
            put_u64(t.resting_id.v);
            put_u64(t.incoming_id.v);
            put_u64((uint64_t)t.px);
            put_u64((uint64_t)t.qty);
        }

        // H_i = SHA256(prev_hash || buf)
        std::vector<uint8_t> hasher_input;
        hasher_input.reserve(prev_hash.size() + buf.size());
        hasher_input.insert(hasher_input.end(), prev_hash.begin(), prev_hash.end());
        hasher_input.insert(hasher_input.end(), buf.begin(), buf.end());
        auto h = SHA256::hash(hasher_input);

        // write: buf + post_hash
        std::fwrite(buf.data(), 1, buf.size(), fp);
        std::fwrite(h.data(), 1, h.size(), fp);
        std::fflush(fp);

        prev_hash = h;
    }

    std::string current_hex_root() const { return hex32(prev_hash); }
};
