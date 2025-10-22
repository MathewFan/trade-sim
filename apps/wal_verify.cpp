#include <bits/stdc++.h>
using namespace std;

static array<uint8_t,32> sha256(const vector<uint8_t>& in); // fwd
// --- minimal SHA-256 (same as engine's) ---
struct SHA256 {
    uint32_t s[8]; uint64_t bitlen=0; uint8_t buf[64]; size_t fill=0;
    static inline uint32_t R(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
    static inline uint32_t Ch(uint32_t x,uint32_t y,uint32_t z){return (x&y)^(~x&z);}
    static inline uint32_t Maj(uint32_t x,uint32_t y,uint32_t z){return (x&y)^(x&z)^(y&z);}
    static inline uint32_t s0(uint32_t x){return R(x,7)^R(x,18)^(x>>3);}
    static inline uint32_t s1(uint32_t x){return R(x,17)^R(x,19)^(x>>10);}
    static inline uint32_t S0(uint32_t x){return R(x,2)^R(x,13)^R(x,22);}
    static inline uint32_t S1(uint32_t x){return R(x,6)^R(x,11)^R(x,25);}
    void init(){ uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(s,iv,32); bitlen=0; fill=0;}
    void tr(const uint8_t d[64]){
        static const uint32_t K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t m[64]; auto U=[&](const uint8_t*p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];};
        for(int i=0;i<16;++i) m[i]=U(d+4*i);
        for(int i=16;i<64;++i) m[i]=s1(m[i-2])+m[i-7]+s0(m[i-15])+m[i-16];
        uint32_t a=s[0],b=s[1],c=s[2],d0=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for(int i=0;i<64;++i){ uint32_t t1=h+S1(e)+Ch(e,f,g)+K[i]+m[i]; uint32_t t2=S0(a)+Maj(a,b,c); h=g; g=f; f=e; e=d0+t1; d0=c; c=b; b=a; a=t1+t2; }
        s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d0; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
    }
    void upd(const uint8_t* p,size_t n){ for(size_t i=0;i<n;++i){ buf[fill++]=p[i]; if(fill==64){ tr(buf); bitlen+=512; fill=0; } } }
    array<uint8_t,32> fin(){ uint64_t bl=bitlen+fill*8; buf[fill++]=0x80; if(fill>56){ while(fill<64) buf[fill++]=0; tr(buf); fill=0; } while(fill<56) buf[fill++]=0; for(int i=7;i>=0;--i) buf[fill++]=(bl>>(8*i))&0xff; tr(buf); array<uint8_t,32> out{}; for(int i=0;i<8;++i){ out[4*i+0]=(s[i]>>24)&0xff; out[4*i+1]=(s[i]>>16)&0xff; out[4*i+2]=(s[i]>>8)&0xff; out[4*i+3]=(s[i])&0xff; } return out; }
}; static array<uint8_t,32> sha256(const vector<uint8_t>& in){ SHA256 h; h.init(); h.upd(in.data(), in.size()); return h.fin(); }

static string hex32(const array<uint8_t,32>& d){ static const char* H="0123456789abcdef"; string s; s.resize(64); for(size_t i=0;i<32;++i){ s[2*i]=H[d[i]>>4]; s[2*i+1]=H[d[i]&0xf]; } return s; }

// big-endian readers (match writer)
static uint64_t be64(const uint8_t* p){ uint64_t v=0; for(int i=0;i<8;++i){ v=(v<<8)|p[i]; } return v; }
static uint32_t be32(const uint8_t* p){ uint32_t v=0; for(int i=0;i<4;++i){ v=(v<<8)|p[i]; } return v; }

int main(int argc, char** argv){
    if(argc<2){ cerr<<"usage: wal_verify <wal_file>\n"; return 1; }
    ifstream f(argv[1], ios::binary);
    if(!f){ cerr<<"cannot open "<<argv[1]<<"\n"; return 1; }

    array<uint8_t,32> prev{}; // zeros
    size_t recs=0;

    while(true){
        // peek to see if EOF
        int c=f.peek(); if(c==EOF) break;

        // Read header: type(1)+seq(8)+ts(8)+raw_len(4)
        vector<uint8_t> buf;
        buf.reserve(64);
        uint8_t h1[1]; uint8_t s8[8]; uint8_t t8[8]; uint8_t rl4[4];
        if(!f.read((char*)h1,1)) break;
        if(!f.read((char*)s8,8)) break;
        if(!f.read((char*)t8,8)) break;
        if(!f.read((char*)rl4,4)) break;
        buf.insert(buf.end(), h1,h1+1);
        buf.insert(buf.end(), s8,s8+8);
        buf.insert(buf.end(), t8,t8+8);
        buf.insert(buf.end(), rl4,rl4+4);

        uint32_t raw_len = be32(rl4);
        vector<uint8_t> raw(raw_len);
        if(raw_len && !f.read((char*)raw.data(), raw_len)) break;
        buf.insert(buf.end(), raw.begin(), raw.end());

        // trade_count(4) + trades
        uint8_t tc4[4]; if(!f.read((char*)tc4,4)) break;
        buf.insert(buf.end(), tc4, tc4+4);
        uint32_t tcnt = be32(tc4);
        size_t trade_bytes = tcnt * (8+8+8+8);
        vector<uint8_t> tbuf(trade_bytes);
        if(trade_bytes && !f.read((char*)tbuf.data(), trade_bytes)) break;
        buf.insert(buf.end(), tbuf.begin(), tbuf.end());

        // read post_hash(32)
        array<uint8_t,32> post{};
        if(!f.read((char*)post.data(), 32)) break;

        // compute H_i = sha256(prev || buf)
        vector<uint8_t> in; in.reserve(prev.size()+buf.size());
        in.insert(in.end(), prev.begin(), prev.end());
        in.insert(in.end(), buf.begin(), buf.end());
        auto computed = sha256(in);
        if(computed!=post){
            cerr<<"Hash mismatch at record "<<recs<<"!\n";
            cerr<<" expected: "<<hex32(post)<<"\n";
            cerr<<" computed: "<<hex32(computed)<<"\n";
            return 2;
        }
        prev = post;
        ++recs;
    }

    cout<<"OK records="<<recs<<" final_root="<<hex32(prev)<<"\n";
    return 0;
}
