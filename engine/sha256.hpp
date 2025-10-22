#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct SHA256 {
    static constexpr size_t DIGEST_SIZE = 32;
    uint32_t state[8];
    uint64_t bitlen = 0;
    uint8_t  data[64];
    size_t   datalen = 0;

    SHA256(){ init(); }

    static inline uint32_t rotr(uint32_t x, uint32_t n){ return (x>>n) | (x<<(32-n)); }
    static inline uint32_t ch(uint32_t x,uint32_t y,uint32_t z){ return (x & y) ^ (~x & z); }
    static inline uint32_t maj(uint32_t x,uint32_t y,uint32_t z){ return (x & y) ^ (x & z) ^ (y & z); }
    static inline uint32_t bsig0(uint32_t x){ return rotr(x,2) ^ rotr(x,13) ^ rotr(x,22); }
    static inline uint32_t bsig1(uint32_t x){ return rotr(x,6) ^ rotr(x,11) ^ rotr(x,25); }
    static inline uint32_t ssig0(uint32_t x){ return rotr(x,7) ^ rotr(x,18) ^ (x>>3); }
    static inline uint32_t ssig1(uint32_t x){ return rotr(x,17) ^ rotr(x,19) ^ (x>>10); }

    void init(){
        state[0]=0x6a09e667; state[1]=0xbb67ae85; state[2]=0x3c6ef372; state[3]=0xa54ff53a;
        state[4]=0x510e527f; state[5]=0x9b05688c; state[6]=0x1f83d9ab; state[7]=0x5be0cd19;
        bitlen=0; datalen=0;
    }

    static inline void to_bytes(uint32_t val, uint8_t* out){
        out[0]=(val>>24)&0xff; out[1]=(val>>16)&0xff; out[2]=(val>>8)&0xff; out[3]=val&0xff;
    }
    static inline uint32_t to_uint32(const uint8_t* in){
        return (uint32_t(in[0])<<24)|(uint32_t(in[1])<<16)|(uint32_t(in[2])<<8)|uint32_t(in[3]);
    }

    void transform(const uint8_t data_block[64]){
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t m[64];
        for(int i=0;i<16;++i) m[i]=to_uint32(data_block+4*i);
        for(int i=16;i<64;++i) m[i]=ssig1(m[i-2])+m[i-7]+ssig0(m[i-15])+m[i-16];

        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for(int i=0;i<64;++i){
            uint32_t t1 = h + bsig1(e) + ch(e,f,g) + K[i] + m[i];
            uint32_t t2 = bsig0(a) + maj(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }

    void update(const uint8_t* data_in, size_t len){
        for(size_t i=0;i<len;++i){
            data[datalen++] = data_in[i];
            if(datalen==64){
                transform(data);
                bitlen += 512;
                datalen=0;
            }
        }
    }
    void update(const std::string& s){ update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    void update(const std::vector<uint8_t>& v){ update(v.data(), v.size()); }

    std::array<uint8_t, DIGEST_SIZE> digest(){
        uint64_t bitlen_total = bitlen + datalen*8;
        // pad
        data[datalen++] = 0x80;
        if(datalen > 56){
            while(datalen<64) data[datalen++]=0x00;
            transform(data); datalen=0;
        }
        while(datalen<56) data[datalen++]=0x00;
        // append length in bits (big endian)
        for(int i=7;i>=0;--i) data[datalen++] = (bitlen_total >> (i*8)) & 0xff;
        transform(data);

        std::array<uint8_t, DIGEST_SIZE> out{};
        for(int i=0;i<8;++i){
            out[4*i+0]=(state[i]>>24)&0xff;
            out[4*i+1]=(state[i]>>16)&0xff;
            out[4*i+2]=(state[i]>>8)&0xff;
            out[4*i+3]=(state[i])&0xff;
        }
        return out;
    }

    static std::array<uint8_t,DIGEST_SIZE> hash(const uint8_t* p, size_t n){ SHA256 h; h.update(p,n); return h.digest(); }
    static std::array<uint8_t,DIGEST_SIZE> hash(const std::vector<uint8_t>& v){ return hash(v.data(), v.size()); }
};
inline std::string hex32(const std::array<uint8_t,32>& d){
    static const char* hexd="0123456789abcdef";
    std::string s; s.resize(64);
    for(size_t i=0;i<32;++i){ s[2*i]=hexd[d[i]>>4]; s[2*i+1]=hexd[d[i]&0xF]; }
    return s;
}
