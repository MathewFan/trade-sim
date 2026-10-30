#include "engine.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <unistd.h>
#include <sys/wait.h>
#define CHECK(x) do{if(!(x)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x);}while(false)
namespace fs=std::filesystem;
struct TempDir {fs::path path=fs::temp_directory_path()/("tradesim-test-"+std::to_string(::getpid())+"-"+std::to_string(std::random_device{}()));TempDir(){fs::create_directories(path);}~TempDir(){fs::remove_all(path);}};
void must_fail(const std::function<void()>& action) {bool failed=false;try{action();}catch(const std::exception&){failed=true;}CHECK(failed);}
std::vector<uint8_t> read(const fs::path& file) {std::ifstream input(file,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void write(const fs::path& file,const std::vector<uint8_t>& bytes) {std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
void recovery() {
    TempDir dir;std::string state,tip;uint64_t highest=0,cancelled=0;
    {
        Engine engine(2,{dir.path.string(),true});engine.start();
        auto first=engine.submit("AAA",Side::Sell,100,10,Tif::Day);first.completion.get();
        auto second=engine.submit("AAA",Side::Buy,100,4,Tif::IOC);CHECK(second.completion.get().trades[0].qty==4);
        auto other=engine.submit("BBB",Side::Buy,20,8,Tif::Day);other.completion.get();
        CHECK(engine.cancel_async(other.id).get().cancelled);cancelled=other.id;highest=other.id;
        const auto snapshot=engine.snapshot("AAA").get();CHECK(snapshot.asks[0].qty==6);state=snapshot.state_hash;tip=snapshot.journal_tip;
        must_fail([&]{Engine duplicate(2,{dir.path.string(),true});duplicate.start();});
        engine.stop();
    }
    {
        Engine recovered(2,{dir.path.string(),true});recovered.start();const auto snapshot=recovered.snapshot("AAA").get();
        CHECK(snapshot.state_hash==state && snapshot.journal_tip==tip && snapshot.asks[0].qty==6);
        CHECK(recovered.snapshot("BBB").get().active_orders==0);
        auto ticket=recovered.submit("AAA",Side::Buy,100,3,Tif::FOK);CHECK(ticket.id>highest);CHECK(ticket.completion.get().trades[0].qty==3);
        CHECK(!recovered.cancel_async(cancelled).get().cancelled);recovered.stop();
    }
    {Engine again(2,{dir.path.string(),false});again.start();CHECK(again.snapshot("AAA").get().asks[0].qty==3);again.stop();}
    must_fail([&]{Engine wrong(3,{dir.path.string(),true});wrong.start();});
    fs::remove(dir.path/"shard1.wal");
    must_fail([&]{Engine partial(2,{dir.path.string(),true});partial.start();});
}
void malformed_logs() {
    TempDir dir;const auto original=dir.path/"good.wal";
    {WalWriter writer(original.string(),1,0,true);WalRecord record;record.seq=1;record.ts_ns=123;record.order={OrderId{1},"ABC",Side::Sell,100,9,123,Tif::Day};writer.append(record);}
    const auto bytes=read(original);CHECK(scan_wal(original.string()).records==1);
    const auto bad=dir.path/"bad.wal";
    for(size_t end=0;end<bytes.size();end++) {
        // A complete header is a valid empty prefix. Detecting deletion of whole
        // trailing records requires an externally trusted expected tip.
        if(end==16) continue;
        write(bad,{bytes.begin(),bytes.begin()+end});must_fail([&]{scan_wal(bad.string());});
    }
    for(size_t offset=0;offset<bytes.size();offset++) {auto corrupt=bytes;corrupt[offset]^=0x40;write(bad,corrupt);must_fail([&]{scan_wal(bad.string());});}
    auto huge=bytes;for(size_t i=16;i<20;i++) huge[i]=0xff;write(bad,huge);must_fail([&]{scan_wal(bad.string());});
    // Frame-boundary truncation passes integrity alone but changes the tip.
    write(bad,{bytes.begin(),bytes.begin()+16});CHECK(scan_wal(bad.string()).tip!=scan_wal(original.string()).tip);
    must_fail([&]{WalWriter writer((dir.path/"missing"/"bad.wal").string(),1,0,true);});
    const auto digest=SHA256::hash(reinterpret_cast<const uint8_t*>("abc"),3);CHECK(hex32(digest)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
void abrupt_exit() {
    TempDir dir;const auto child=::fork();CHECK(child>=0);
    if(child==0) {try{Engine engine(1,{dir.path.string(),true});engine.start();engine.submit("CRASH",Side::Sell,100,9,Tif::Day).completion.get();engine.submit("CRASH",Side::Buy,100,4,Tif::IOC).completion.get();::_exit(0);}catch(...){::_exit(1);}}
    int status=0;CHECK(::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    Engine recovered(1,{dir.path.string(),true});recovered.start();const auto snapshot=recovered.snapshot("CRASH").get();CHECK(snapshot.asks.size()==1 && snapshot.asks[0].qty==5 && snapshot.sequence==2);recovered.stop();
}
int main(){try{recovery();malformed_logs();abrupt_exit();std::cout<<"PASS restart replay, ID continuity, locks, topology, SHA-256 vector, every-byte corruption and truncation\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
