#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "../engine/engine.hpp"
#include "../engine/sha256.hpp" // we reuse base64 from here? (we'll implement a tiny base64 here)

// --- tiny base64 encoder (RFC4648, no line breaks) ---
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
        for(int i=0;i<16;++i) w[i]=(B[4*i]<<24)|(B[4*i+1]<<16)|(B[4*i+2]<<8)|B[4*i+3];
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

// Minimal WebSocket frame send (text only, <= 125 bytes per chunk; will chunk if larger)
static bool ws_send_text(int fd, const std::string& msg){
    size_t i=0;
    while(i<msg.size()){
        size_t chunk = std::min<size_t>(125, msg.size()-i);
        uint8_t hdr[2]; hdr[0]=0x81; hdr[1]=uint8_t(chunk); // FIN+text, no mask (server->client)
        if(::send(fd, hdr, 2, 0)<=0) return false;
        if(::send(fd, msg.data()+i, chunk, 0)<=0) return false;
        i+=chunk;
    }
    return true;
}

// Read a single WS text message (no fragmentation) and return payload string; returns empty string on close/error
static std::string ws_read_text(int fd){
    uint8_t h[2]; ssize_t n=recv(fd,h,2,MSG_WAITALL); if(n!=2) return {};
    bool fin = (h[0] & 0x80)!=0; (void)fin;
    uint8_t opcode = h[0] & 0x0F; if(opcode==0x8) return {}; // close
    bool masked = (h[1] & 0x80)!=0; if(!masked) return {};
    uint64_t len = h[1] & 0x7F;
    if(len==126){ uint8_t ext[2]; if(recv(fd,ext,2,MSG_WAITALL)!=2) return {}; len=(ext[0]<<8)|ext[1]; }
    else if(len==127){ uint8_t ext[8]; if(recv(fd,ext,8,MSG_WAITALL)!=8) return {}; len=0; for(int i=0;i<8;++i) len=(len<<8)|ext[i]; }
    uint8_t mask[4]; if(recv(fd,mask,4,MSG_WAITALL)!=4) return {};
    std::string payload; payload.resize(len);
    if(len && recv(fd,payload.data(),len,MSG_WAITALL)!=(ssize_t)len) return {};
    for(size_t i=0;i<len;++i) payload[i]^=mask[i&3];
    return payload;
}

// Very tiny line-based JSON parser for our 3 commands (we avoid pulling a JSON lib)
static inline std::string get_str(const std::string& s, const char* key){
    auto k = std::string("\"")+key+"\"";
    auto p = s.find(k); if(p==std::string::npos) return {};
    p = s.find(':',p); if(p==std::string::npos) return {};
    p = s.find_first_of("\"", p+1); if(p==std::string::npos) return {};
    auto q = s.find('"', p+1); if(q==std::string::npos) return {};
    return s.substr(p+1, q-(p+1));
}
static inline long long get_num(const std::string& s, const char* key, long long def=0){
    auto k = std::string("\"")+key+"\"";
    auto p = s.find(k); if(p==std::string::npos) return def;
    p = s.find(':',p); if(p==std::string::npos) return def;
    auto q = s.find_first_of(",} \n\r", p+1);
    auto num = s.substr(p+1, (q==std::string::npos? s.size():q) - (p+1));
    try { return std::stoll(num); } catch(...) { return def; }
}

int main(int argc, char** argv){
    int port = 9001;
    size_t nshards = 4;
    if(argc>=2) port = std::stoi(argv[1]);
    if(argc>=3) nshards = std::max<size_t>(1, std::stoul(argv[2]));

    Engine eng(nshards);
    // --- simple event queue for broadcasting from engine thread(s) to ws thread(s)
    std::mutex q_mtx; std::condition_variable q_cv; std::deque<std::string> q;
    eng.set_event_sink([&](const std::string& ev){
        std::lock_guard<std::mutex> lk(q_mtx);
        q.push_back(ev);
        q_cv.notify_all();
    });
    eng.start();

    // --- socket setup
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int one=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
    if(::bind(srv,(sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 1; }
    if(::listen(srv, 8)<0){ perror("listen"); return 1; }
    std::cout<<"WS listening on ws://127.0.0.1:"<<port<<"/ws\n";

    std::vector<int> clients; std::mutex c_mtx;

    // broadcaster thread
    std::thread broadcaster([&](){
        while(true){
            std::unique_lock<std::mutex> lk(q_mtx);
            q_cv.wait(lk, [&]{ return !q.empty(); });
            auto msg = std::move(q.front()); q.pop_front();
            lk.unlock();

            std::lock_guard<std::mutex> ck(c_mtx);
            for(auto it=clients.begin(); it!=clients.end(); ){
                if(!ws_send_text(*it, msg)){ ::close(*it); it=clients.erase(it); }
                else ++it;
            }
        }
    });
    broadcaster.detach();

    // accept loop
    while(true){
        int fd = ::accept(srv,nullptr,nullptr);
        if(fd<0){ perror("accept"); continue; }

        // --- read HTTP request (very naive)
        std::string req; req.resize(4096);
        ssize_t n = recv(fd, req.data(), req.size(), 0);
        if(n<=0){ ::close(fd); continue; }
        req.resize(n);
        if(req.find("Upgrade: websocket")==std::string::npos || req.find("GET /ws ")==std::string::npos){
            const char* bad="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(fd,bad,strlen(bad),0); ::close(fd); continue;
        }
        // find Sec-WebSocket-Key
        std::string key_hdr="Sec-WebSocket-Key:";
        auto kpos=req.find(key_hdr);
        if(kpos==std::string::npos){ ::close(fd); continue; }
        auto kstart=req.find_first_not_of(" \t", kpos+key_hdr.size());
        auto kend=req.find("\r\n",kstart);
        std::string key=req.substr(kstart, kend-kstart);
        // compute accept = base64( SHA1(key + GUID) )
        const std::string GUID="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        auto sha = SHA1::hash(key+GUID);
        std::string accept = b64(sha.data(), sha.size());
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: "+accept+"\r\n\r\n";
        send(fd, resp.data(), resp.size(), 0);

        { std::lock_guard<std::mutex> lk(c_mtx); clients.push_back(fd); }

        // per-client receive loop
        std::thread([fd,&eng](){
            while(true){
                std::string payload = ws_read_text(fd);
                if(payload.empty()){ ::close(fd); break; }
                // parse minimal JSON
                auto type = get_str(payload, "type");
                if(type=="N"){
                    auto sym = get_str(payload, "sym");
                    auto side = get_str(payload, "side");
                    long long px  = get_num(payload, "px");
                    long long qty = get_num(payload, "qty");
                    auto tif_s = get_str(payload, "tif");
                    Tif tif = (tif_s=="IOC")?Tif::IOC : (tif_s=="FOK")?Tif::FOK : Tif::Day;
                    eng.submit_new(sym, (side=="B"?Side::Buy:Side::Sell), px, qty, tif);
                } else if(type=="C"){
                    uint64_t id = (uint64_t)get_num(payload, "id");
                    eng.submit_cancel(id);
                } else if(type=="P"){
                    auto sym = get_str(payload, "sym");
                    eng.submit_print(sym);
                } else {
                    // ignore
                }
            }
        }).detach();
    }

    eng.stop();
    return 0;
}
