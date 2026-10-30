#include "engine.hpp"
#include "protocol.hpp"
#include "websocket.hpp"
#include <arpa/inet.h>
#include <csignal>
#include <deque>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stopped=0;
void signal_stop(int){stopped=1;}
std::string lower(std::string value){for(auto& ch:value)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));return value;}
std::string trim(std::string value){const auto begin=value.find_first_not_of(" \t\r");if(begin==std::string::npos)return {};return value.substr(begin,value.find_last_not_of(" \t\r")-begin+1);}
bool token(const std::string& value,const std::string& target){std::istringstream input(lower(value));std::string item;while(std::getline(input,item,','))if(trim(item)==target)return true;return false;}
struct Pending {char type;std::future<ExecutionReport> execution;std::future<Snapshot> snapshot;};
struct Client {
    int fd;bool handshake{},dead{};std::string input,output;size_t sent{};
    std::deque<Pending> pending;std::chrono::steady_clock::time_point connected=std::chrono::steady_clock::now();
    explicit Client(int socket):fd(socket){}
};
void queue(Client& client,const std::string& payload,uint8_t opcode=1) {
    if(client.output.size()-client.sent+payload.size()>262144) {client.dead=true;return;}
    if(client.sent){client.output.erase(0,client.sent);client.sent=0;}
    client.output+=websocket::frame(payload,opcode);
}
void invalid(Client& client){queue(client,"{\"event\":\"error\",\"message\":\"Invalid command or engine unavailable\"}");}
void upgrade(Client& client){
    const auto end=client.input.find("\r\n\r\n");if(end==std::string::npos)return;
    std::istringstream request(client.input.substr(0,end));std::string line;std::getline(request,line);
    if(trim(line)!="GET /ws HTTP/1.1")throw std::runtime_error("unsupported request path");
    std::map<std::string,std::string> headers;
    while(std::getline(request,line)){const auto colon=line.find(':');if(colon==std::string::npos)throw std::runtime_error("bad HTTP header");if(!headers.emplace(lower(trim(line.substr(0,colon))),trim(line.substr(colon+1))).second)throw std::runtime_error("duplicate HTTP header");}
    const auto key=headers["sec-websocket-key"];
    if(lower(headers["upgrade"])!="websocket"||!token(headers["connection"],"upgrade")||headers["sec-websocket-version"]!="13"||key.size()!=24||key.substr(22)!="==")throw std::runtime_error("invalid WebSocket handshake");
    for(size_t i=0;i<22;i++)if(!(std::isalnum(static_cast<unsigned char>(key[i]))||key[i]=='+'||key[i]=='/'))throw std::runtime_error("invalid WebSocket key");
    client.output="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+websocket::accept_key(key)+"\r\n\r\n";
    client.input.erase(0,end+4);client.handshake=true;
}
std::string depth_json(const Snapshot& value) {
    auto levels=[](const auto& side){std::string out="[";for(size_t i=0;i<std::min<size_t>(10,side.size());i++){if(i)out+=",";out+="["+std::to_string(side[i].px)+","+std::to_string(side[i].qty)+","+std::to_string(side[i].orders)+"]";}return out+"]";};
    return "{\"event\":\"depth\",\"sym\":\""+value.sym+"\",\"seq\":"+std::to_string(value.sequence)+",\"active_orders\":"+std::to_string(value.active_orders)+",\"bids\":"+levels(value.bids)+",\"asks\":"+levels(value.asks)+",\"state_hash\":\""+value.state_hash+"\",\"journal_tip\":\""+value.journal_tip+"\"}";
}
void command(Client& client,Engine& engine,const std::string& payload) {
    try {
        if(client.pending.size()>=128)throw std::runtime_error("too many pending commands");
        const auto cmd=protocol::command(payload);Pending pending;pending.type=cmd.type;
        if(cmd.type=='N')pending.execution=engine.submit(cmd.sym,cmd.side,cmd.px,cmd.qty,cmd.tif).completion;
        else if(cmd.type=='C')pending.execution=engine.cancel_async(cmd.id);
        else pending.snapshot=engine.snapshot(cmd.sym);
        client.pending.push_back(std::move(pending));
    }catch(const std::exception&){invalid(client);}
}
void completions(Client& client) {
    while(!client.pending.empty()) {
        auto& pending=client.pending.front();
        const bool ready=pending.type=='P'?pending.snapshot.wait_for(std::chrono::seconds(0))==std::future_status::ready:pending.execution.wait_for(std::chrono::seconds(0))==std::future_status::ready;
        if(!ready)break;
        try {
            if(pending.type=='P')queue(client,depth_json(pending.snapshot.get()));
            else {const auto report=pending.execution.get();queue(client,"{\"event\":\"ack\",\"action\":\""+std::string(1,pending.type)+"\",\"id\":"+std::to_string(report.id)+",\"seq\":"+std::to_string(report.sequence)+",\"remaining\":"+std::to_string(report.remaining)+",\"resting\":"+(report.resting?"true":"false")+",\"cancelled\":"+(report.cancelled?"true":"false")+"}");}
        }catch(const std::exception&){invalid(client);}
        client.pending.pop_front();
    }
}
}
int main(int argc,char** argv) {
    int listener=-1;std::vector<Client> clients;
    try {
        const int port=argc>1?std::stoi(argv[1]):9001;if(port<0||port>65535)throw std::invalid_argument("invalid port");
        std::signal(SIGPIPE,SIG_IGN);std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
        std::mutex events_mutex;std::deque<std::string> events;size_t dropped_events=0;
        Engine engine(argc>2?std::stoul(argv[2]):4,EngineOptions{argc>3?argv[3]:"",true});
        engine.set_event_sink([&](const std::string& value){std::lock_guard<std::mutex> lock(events_mutex);if(events.size()==4096){++dropped_events;return;}events.push_back(value);});
        engine.start();listener=::socket(AF_INET,SOCK_STREAM,0);if(listener<0)throw std::runtime_error("socket failed");
        int one=1;::setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));::fcntl(listener,F_SETFL,O_NONBLOCK);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(static_cast<uint16_t>(port));
        if(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0||::listen(listener,64)<0)throw std::runtime_error("bind/listen failed");
        socklen_t size=sizeof(address);::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);
        std::cout<<"WS listening on ws://127.0.0.1:"<<ntohs(address.sin_port)<<"/ws"<<std::endl;
        while(!stopped) {
            std::vector<pollfd> descriptors{{listener,POLLIN,0}};
            for(auto& client:clients)descriptors.push_back({client.fd,static_cast<short>(POLLIN|(client.sent<client.output.size()?POLLOUT:0)),0});
            const auto count=clients.size();const int polled=::poll(descriptors.data(),descriptors.size(),5);
            if(polled<0){if(errno==EINTR)continue;throw std::runtime_error("poll failed");}
            if(descriptors[0].revents&POLLIN) {
                const int fd=::accept(listener,nullptr,nullptr);
                if(fd>=0){if(clients.size()>=64)::close(fd);else{::fcntl(fd,F_SETFL,O_NONBLOCK);clients.emplace_back(fd);}}
            }
            for(size_t i=0;i<count;i++) {
                auto& client=clients[i];const auto flags=descriptors[i+1].revents;
                if(flags&(POLLERR|POLLHUP|POLLNVAL)){client.dead=true;continue;}
                if(flags&POLLIN) {
                    char buffer[4096];const auto n=::recv(client.fd,buffer,sizeof(buffer),0);
                    if(n==0 || (n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))client.dead=true;
                    if(n>0)client.input.append(buffer,static_cast<size_t>(n));
                }
                try {
                    if(client.input.size()>8192)throw std::runtime_error("receive buffer exceeded");
                    if(!client.handshake) {
                        if(std::chrono::steady_clock::now()-client.connected>std::chrono::seconds(2))throw std::runtime_error("handshake timeout");
                        upgrade(client);
                    }
                    if(client.handshake) {
                        while(auto frame=websocket::parse(client.input)) {
                            if(frame->opcode==8){client.dead=true;break;}
                            if(frame->opcode==9)queue(client,frame->payload,10);
                            else if(frame->opcode==1)command(client,engine,frame->payload);
                        }
                        completions(client);
                    }
                }catch(const std::exception&){client.dead=true;}
                if(!client.dead && client.sent<client.output.size()) {
                    const auto n=::send(client.fd,client.output.data()+client.sent,client.output.size()-client.sent,0);
                    if(n>0)client.sent+=static_cast<size_t>(n);
                    else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)client.dead=true;
                    if(client.sent==client.output.size()){client.output.clear();client.sent=0;}
                }
            }
            std::deque<std::string> batch;{std::lock_guard<std::mutex> lock(events_mutex);batch.swap(events);}
            for(const auto& event:batch)for(auto& client:clients)if(client.handshake&&!client.dead)queue(client,event);
            for(auto it=clients.begin();it!=clients.end();) {if(it->dead){::close(it->fd);it=clients.erase(it);}else ++it;}
        }
        engine.stop();std::cout<<"dropped_trade_events="<<dropped_events<<'\n';
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';if(listener>=0)::close(listener);for(auto& client:clients)::close(client.fd);return 1;}
    if(listener>=0)::close(listener);for(auto& client:clients)::close(client.fd);
}
