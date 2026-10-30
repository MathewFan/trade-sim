#include "wal.hpp"
#include <iostream>
int main(int argc,char** argv) {
    if(argc<2 || argc>3) {std::cerr<<"usage: wal_verify FILE [TRUSTED_EXPECTED_TIP]\n";return 1;}
    try {
        const auto summary=scan_wal(argv[1]);const auto tip=hex32(summary.tip);
        if(argc==3 && tip!=argv[2]) throw std::runtime_error("WAL tip differs from trusted checkpoint");
        std::cout<<"OK records="<<summary.records<<" shards="<<summary.shards<<" shard="<<summary.index<<" journal_tip="<<tip<<'\n';
    }catch(const std::exception& error) {std::cerr<<"INVALID "<<error.what()<<'\n';return 2;}
}
