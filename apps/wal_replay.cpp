#include "engine.hpp"
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=4) {std::cerr<<"usage: wal_replay DIRECTORY SHARDS SYMBOL\n";return 1;}
    try {
        Engine engine(std::stoul(argv[2]),EngineOptions{argv[1],true});engine.start();
        const auto snapshot=engine.snapshot(argv[3]).get();engine.stop();
        std::cout<<snapshot.canonical<<"STATE_HASH "<<snapshot.state_hash<<"\nJOURNAL_TIP "<<snapshot.journal_tip<<"\nSEQUENCE "<<snapshot.sequence<<'\n';
    }catch(const std::exception& error) {std::cerr<<"REPLAY FAILED "<<error.what()<<'\n';return 2;}
}
