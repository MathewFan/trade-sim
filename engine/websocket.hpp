#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace websocket {
static std::string b64(const uint8_t* data, size_t len){
    static const char* T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((len+2)/3*4);
    size_t i=0; while(i+3<=len){
        uint32_t v=(data[i]<<16)|(data[i+1]<<8)|data[i+2];
        out.push_back(T[(v>>18)&63]); out.push_back(T[(v>>12)&63]);
        out.push_back(T[(v>>6)&63]);  out.push_back(T[v&63]);
        i+=3;
    }
    if(i+1==len){
        uint32_t v=(data[i]<<16);
        out.push_back(T[(v>>18)&63]); out.push_back(T[(v>>12)&63]);
        out.push_back('=' ); out.push_back('=');
    } else if(i+2==len){
        uint32_t v=(data[i]<<16)|(data[i+1]<<8);
        out.push_back(T[(v>>18)&63]); out.push_back(T[(v>>12)&63]);
        out.push_back(T[(v>>6)&63]);  out.push_back('=');
    }
    return out;
}

// SHA1 (minimal, for WS accept key)
struct SHA1 {
    uint32_t h[5]; uint64_t bits=0; uint8_t buf[64]; size_t fill=0;
    static inline uint32_t rol(uint32_t v,int n){ return (v<<n)|(v>>(32-n)); }
    void init(){ h[0]=0x67452301; h[1]=0xEFCDAB89; h[2]=0x98BADCFE; h[3]=0x10325476; h[4]=0xC3D2E1F0; bits=0; fill=0; }
    void tr(const uint8_t B[64]){
        uint32_t w[80];
        for(int i=0;i<16;++i) w[i]=(uint32_t(B[4*i])<<24)|(uint32_t(B[4*i+1])<<16)|(uint32_t(B[4*i+2])<<8)|uint32_t(B[4*i+3]);
        for(int i=16;i<80;++i) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;++i){
            uint32_t f,k;
            if(i<20){ f=(b&c)|((~b)&d); k=0x5A827999; }
            else if(i<40){ f=b^c^d; k=0x6ED9EBA1; }
            else if(i<60){ f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            uint32_t t=rol(a,5)+f+e+k+w[i];
            e=d; d=c; c=rol(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    void update(const uint8_t* p,size_t n){
        for(size_t i=0;i<n;++i){
            buf[fill++]=p[i];
            if(fill==64){ tr(buf); bits+=512; fill=0; }
        }
    }
    std::array<uint8_t,20> finish(){
        uint64_t total=bits+fill*8;
        buf[fill++]=0x80;
        if(fill>56){ while(fill<64) buf[fill++]=0; tr(buf); fill=0; }
        while(fill<56) buf[fill++]=0;
        for(int i=7;i>=0;--i) buf[fill++]=(total>>(8*i))&0xff;
        tr(buf);
        std::array<uint8_t,20> out{};
        for(int i=0;i<5;++i){ out[4*i+0]=(h[i]>>24)&0xff; out[4*i+1]=(h[i]>>16)&0xff; out[4*i+2]=(h[i]>>8)&0xff; out[4*i+3]=h[i]&0xff; }
        return out;
    }
    static std::array<uint8_t,20> hash(const std::string& s){ SHA1 sh; sh.init(); sh.update((const uint8_t*)s.data(), s.size()); return sh.finish(); }
};


inline std::string accept_key(const std::string& key) {
    auto digest=SHA1::hash(key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");return b64(digest.data(),digest.size());
}
struct Frame {uint8_t opcode;std::string payload;};
inline std::optional<Frame> parse(std::string& buffer) {
    if(buffer.size()<2) return std::nullopt;
    const auto first=static_cast<uint8_t>(buffer[0]),second=static_cast<uint8_t>(buffer[1]);
    const auto opcode=first&15;
    if((first&0xf0)!=0x80 || !(second&0x80) || (opcode!=1&&opcode!=8&&opcode!=9&&opcode!=10)) throw std::runtime_error("unsupported/unmasked WebSocket frame");
    uint64_t length=second&127;size_t header=2;
    if(length==126) {if(buffer.size()<4)return std::nullopt;length=(static_cast<uint8_t>(buffer[2])<<8)|static_cast<uint8_t>(buffer[3]);header=4;if(length<126)throw std::runtime_error("noncanonical WebSocket length");}
    else if(length==127) {if(buffer.size()<10)return std::nullopt;length=0;for(size_t i=2;i<10;i++)length=(length<<8)|static_cast<uint8_t>(buffer[i]);header=10;if(length<65536)throw std::runtime_error("noncanonical WebSocket length");}
    if(length>4096 || (opcode>=8&&length>125)) throw std::runtime_error("WebSocket payload limit exceeded");
    if(buffer.size()<header+4+length) return std::nullopt;
    std::string payload=buffer.substr(header+4,static_cast<size_t>(length));
    for(size_t i=0;i<length;i++)payload[i]^=buffer[header+(i%4)];
    buffer.erase(0,header+4+static_cast<size_t>(length));return Frame{static_cast<uint8_t>(opcode),std::move(payload)};
}
inline std::string frame(const std::string& payload,uint8_t opcode=1) {
    std::string out;out.push_back(static_cast<char>(0x80|opcode));
    if(payload.size()<126)out.push_back(static_cast<char>(payload.size()));
    else if(payload.size()<=65535){out.push_back(126);out.push_back(static_cast<char>(payload.size()>>8));out.push_back(static_cast<char>(payload.size()));}
    else throw std::length_error("outgoing WebSocket message too large");
    out+=payload;return out;
}
}
