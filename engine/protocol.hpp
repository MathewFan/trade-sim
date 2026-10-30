#pragma once
#include "book.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <variant>
namespace protocol {
using Value=std::variant<std::string,long long>;
using Object=std::map<std::string,Value>;
class Parser {
    const std::string& input;size_t pos{};
    char take(){if(pos==input.size())throw std::invalid_argument("incomplete JSON");return input[pos++];}
    void whitespace(){while(pos<input.size()&&(input[pos]==' '||input[pos]=='\n'||input[pos]=='\r'||input[pos]=='\t'))pos++;}
    std::string string() {
        if(take()!='"')throw std::invalid_argument("expected JSON string");std::string out;
        while(true){auto ch=take();if(ch=='"')return out;if(static_cast<unsigned char>(ch)<32)throw std::invalid_argument("control character in JSON");
            if(ch=='\\'){ch=take();switch(ch){case '"':case '\\':case '/':break;case 'b':ch='\b';break;case 'f':ch='\f';break;case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;default:throw std::invalid_argument("unsupported JSON escape; use ASCII command fields");}}out.push_back(ch);}
    }
    Value value() {
        if(pos<input.size()&&input[pos]=='"')return string();
        const auto start=pos;if(pos<input.size()&&input[pos]=='-')pos++;
        if(pos==input.size()||!std::isdigit(static_cast<unsigned char>(input[pos])))throw std::invalid_argument("expected integer");
        if(input[pos]=='0')pos++;else while(pos<input.size()&&std::isdigit(static_cast<unsigned char>(input[pos])))pos++;
        long long result{};const auto parsed=std::from_chars(input.data()+start,input.data()+pos,result);
        if(parsed.ec!=std::errc{}||parsed.ptr!=input.data()+pos)throw std::invalid_argument("integer outside int64 range");return result;
    }
public:
    explicit Parser(const std::string& text):input(text){}
    Object parse(){if(input.size()>4096)throw std::invalid_argument("JSON too large");Object out;whitespace();if(take()!='{')throw std::invalid_argument("expected JSON object");whitespace();
        if(pos<input.size()&&input[pos]=='}')pos++;
        else while(true){whitespace();auto key=string();whitespace();if(take()!=':')throw std::invalid_argument("expected colon");whitespace();auto val=value();if(out.size()>=8||!out.emplace(key,std::move(val)).second)throw std::invalid_argument("duplicate key or too many fields");whitespace();const auto next=take();if(next=='}')break;if(next!=',')throw std::invalid_argument("expected comma");}
        whitespace();if(pos!=input.size())throw std::invalid_argument("trailing JSON data");return out;}
};
inline std::string text(const Object& object,const std::string& key) {const auto found=object.find(key);if(found==object.end()||!std::holds_alternative<std::string>(found->second))throw std::invalid_argument("missing/string field "+key);return std::get<std::string>(found->second);}
inline long long integer(const Object& object,const std::string& key) {const auto found=object.find(key);if(found==object.end()||!std::holds_alternative<long long>(found->second))throw std::invalid_argument("missing/integer field "+key);return std::get<long long>(found->second);}
struct Command {char type;std::string sym;Side side{};Tif tif{Tif::Day};long long px{},qty{};uint64_t id{};};
inline Command command(const std::string& payload) {
    const auto object=Parser(payload).parse();const auto type=text(object,"type");Command result{};
    std::vector<std::string> allowed{"type"};
    if(type=="N") {
        result.type='N';result.sym=text(object,"sym");const auto side=text(object,"side");if(side!="B"&&side!="S")throw std::invalid_argument("side must be B or S");result.side=side=="B"?Side::Buy:Side::Sell;
        result.px=integer(object,"px");result.qty=integer(object,"qty");const auto tif=object.count("tif")?text(object,"tif"):"DAY";
        if(tif!="DAY"&&tif!="IOC"&&tif!="FOK")throw std::invalid_argument("invalid TIF");result.tif=tif=="DAY"?Tif::Day:tif=="IOC"?Tif::IOC:Tif::FOK;
        validate_order({OrderId{1},result.sym,result.side,result.px,result.qty,0,result.tif});allowed={"type","sym","side","px","qty","tif"};
    }else if(type=="C") {result.type='C';const auto id=integer(object,"id");if(id<=0)throw std::invalid_argument("ID must be positive");result.id=static_cast<uint64_t>(id);allowed={"type","id"};}
    else if(type=="P") {result.type='P';result.sym=text(object,"sym");validate_symbol(result.sym);allowed={"type","sym"};}
    else throw std::invalid_argument("unknown command type");
    for(const auto& [key,value]:object){(void)value;if(std::find(allowed.begin(),allowed.end(),key)==allowed.end())throw std::invalid_argument("unknown command field");}
    return result;
}
}
