#include "protocol.hpp"
#include "websocket.hpp"
#include <iostream>
#include <functional>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
void rejected(const std::function<void()>& action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}CHECK(failed);}
std::string masked(const std::string& value,uint8_t opcode=1){std::string out;out.push_back(static_cast<char>(0x80|opcode));if(value.size()<126)out.push_back(static_cast<char>(0x80|value.size()));else{out.push_back(static_cast<char>(0xfe));out.push_back(static_cast<char>(value.size()>>8));out.push_back(static_cast<char>(value.size()));}out+="abcd";for(size_t i=0;i<value.size();i++)out.push_back(value[i]^"abcd"[i%4]);return out;}
int main(){try{
    CHECK(websocket::accept_key("dGhlIHNhbXBsZSBub25jZQ==")=="s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    for(const auto& value:{std::string("hello"),std::string(300,'z')}){
        const auto frame=masked(value);
        for(size_t cut=0;cut<frame.size();cut++){auto partial=frame.substr(0,cut);CHECK(!websocket::parse(partial));partial+=frame.substr(cut);CHECK(websocket::parse(partial)->payload==value && partial.empty());}
    }
    auto combined=masked("a")+masked("b",9);CHECK(websocket::parse(combined)->payload=="a");CHECK(websocket::parse(combined)->opcode==9);
    for(auto frame:{std::string("\x81\x01x",3),std::string("\x81\xff",2)+std::string(8,'\xff'),std::string("\x01\x80" "abcd",6),std::string("\x89\xfe\x00\x7e",4)})rejected([&]{websocket::parse(frame);});
    CHECK(websocket::frame(std::string(300,'a')).size()==304);
    const auto parsed=protocol::command("{\"type\":\"N\",\"sym\":\"BTC-USD\",\"side\":\"B\",\"px\":100,\"qty\":4}");CHECK(parsed.tif==Tif::Day && parsed.qty==4);
    CHECK(protocol::command("{\"type\":\"C\",\"id\": 42}").id==42);
    for(const std::string value:{"{}","{\"type\":\"P\",\"sym\":\"A\",\"sym\":\"B\"}","{\"type\":\"C\",\"id\":-1}","{\"type\":\"C\",\"id\":01}","{\"type\":\"C\",\"id\":1.5}","{\"type\":\"C\",\"id\":9999999999999999999999999}","{\"type\":\"N\",\"sym\":\"X\",\"side\":\"Z\",\"px\":1,\"qty\":2}","{\"type\":\"P\",\"sym\":\"A\",\"extra\":1}","{\"type\":\"P\",\"sym\":\"A\"}garbage"})rejected([&]{protocol::command(value);});
    std::cout<<"PASS RFC handshake vector, every frame split, bounded frames, strict JSON validation\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
