#include <bits/stdc++.h>
#include "../engine/engine.hpp"

int main(int argc, char** argv){
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    size_t nshards = 4; // tweak as you like: 2, 4, 8...
    if (argc>=2) nshards = std::max<size_t>(1, std::stoul(argv[1]));

    Engine eng(nshards);
    eng.start();

    std::string cmd;
    while (std::cin >> cmd){
        if (cmd=="N"){
            std::string sym, side_s, tif_s="DAY"; long long px, qty;
            std::cin >> sym >> side_s >> px >> qty >> tif_s;
            Side side = (side_s=="B")? Side::Buy : Side::Sell;
            Tif  tif  = (tif_s=="IOC")? Tif::IOC : (tif_s=="FOK")? Tif::FOK : Tif::Day;
            eng.submit_new(sym, side, px, qty, tif);
        } else if (cmd=="C"){
            uint64_t id; std::cin >> id;
            eng.submit_cancel(id);
        } else if (cmd=="P"){
            std::string sym; std::cin >> sym;
            eng.submit_print(sym);
        }
    }

    eng.stop();
    return 0;
}
