#include "engine.hpp"
#include <iostream>
#include <sstream>
int main(int argc,char** argv) {
    try {
        Engine engine(argc>1?std::stoul(argv[1]):4,EngineOptions{argc>2?argv[2]:"",true});engine.set_event_sink([](const std::string& event){std::cout<<event<<'\n';});engine.start();
        std::string line;
        while(std::getline(std::cin,line)) {
            std::istringstream input(line);std::string command;input>>command;
            try {
                if(command=="N") {
                    std::string sym,side,tif;long long px,qty;
                    if(!(input>>sym>>side>>px>>qty>>tif)||(side!="B"&&side!="S")||(tif!="DAY"&&tif!="IOC"&&tif!="FOK")) throw std::invalid_argument("N SYMBOL B|S PRICE QTY DAY|IOC|FOK");
                    auto ticket=engine.submit(sym,side=="B"?Side::Buy:Side::Sell,px,qty,tif=="DAY"?Tif::Day:tif=="IOC"?Tif::IOC:Tif::FOK);
                    auto report=ticket.completion.get();std::cout<<"ACK "<<report.id<<" seq="<<report.sequence<<" remaining="<<report.remaining<<" resting="<<report.resting<<'\n';
                }else if(command=="C") {uint64_t id;if(!(input>>id)) throw std::invalid_argument("C ID");std::cout<<"CANCEL "<<engine.cancel_async(id).get().cancelled<<'\n';}
                else if(command=="P") {std::string sym;if(!(input>>sym)) throw std::invalid_argument("P SYMBOL");engine.submit_print(sym);engine.flush();}
                else if(!command.empty()) throw std::invalid_argument("unknown command");
            }catch(const std::exception& error){std::cerr<<"ERROR "<<error.what()<<'\n';}
        }
        engine.flush();engine.stop();
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
