#include "engine.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
long long now_ns() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
void fail_promises(Msg& msg,std::exception_ptr error) {
    if(msg.report) msg.report->set_exception(error);
    if(msg.snapshot) msg.snapshot->set_exception(error);
    if(msg.barrier) msg.barrier->set_exception(error);
}
}
Engine::Engine(size_t count,EngineOptions opts):nshards(count),options(std::move(opts)) {
    if(!count || count>64) throw std::invalid_argument("shards must be 1..64");
    for(size_t i=0;i<count;++i) shards.push_back(std::make_unique<ShardWorker>());
}
Engine::~Engine() {try {stop();}catch(...) {}}
void Engine::check_failure() {std::lock_guard<std::mutex> lock(failure_mutex);if(failure) std::rethrow_exception(failure);}
void Engine::set_event_sink(std::function<void(const std::string&)> sink) {
    std::unique_lock<std::shared_mutex> lock(lifecycle);
    if(started) throw std::logic_error("configure sink before start"); on_event=std::move(sink);
}
void Engine::emit(const std::string& event) {
    if(!on_event) return;
    std::lock_guard<std::mutex> lock(sink_mutex);
    try {on_event(event);}catch(...) {/* Observers cannot corrupt matching state. */}
}
void Engine::start() {
    std::unique_lock<std::shared_mutex> lock(lifecycle);
    if(started) throw std::logic_error("an engine instance may start only once");
    started=true;
    if(!options.wal_directory.empty()) {
        std::filesystem::create_directories(options.wal_directory);
        for(size_t i=0;i<nshards;++i) shards[i]->wal=std::make_unique<WalWriter>(options.wal_directory+"/shard"+std::to_string(i)+".bin");
    }
    try {
        for(auto& shard:shards) {shard->running=true;auto* ptr=shard.get();shard->thread=std::thread([this,ptr]{worker(*ptr);});}
        accepting=true;
    }catch(...) {
        for(auto& shard:shards) shard->running=false;
        for(auto& shard:shards) if(shard->thread.joinable()) shard->thread.join();
        throw;
    }
}
void Engine::stop() {
    std::unique_lock<std::shared_mutex> lock(lifecycle);
    accepting=false;
    for(auto& shard:shards) shard->running=false;
    for(auto& shard:shards) if(shard->thread.joinable()) shard->thread.join();
}
void Engine::enqueue(size_t index,Msg msg) {
    std::shared_lock<std::shared_mutex> lock(lifecycle);
    if(!accepting) throw std::logic_error("engine is not accepting commands");
    check_failure();
    auto& shard=*shards[index];
    // Serializing producers preserves the ring's single-producer contract.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    std::unique_lock<std::timed_mutex> producer(shard.producer_mutex,std::defer_lock);
    if(!producer.try_lock_until(deadline)) throw std::runtime_error("shard producer admission timed out");
    while(!shard.ring.push(msg)) {
        check_failure();
        if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("shard queue admission timed out");
        std::this_thread::yield();
    }
}
Ticket Engine::submit(const std::string& sym,Side side,long long px,long long qty,Tif tif) {
    validate_symbol(sym);
    const auto serial=next_id.fetch_add(1);
    if(serial>(std::numeric_limits<uint64_t>::max()-nshards)/nshards) throw std::overflow_error("order IDs exhausted");
    const auto index=shard_of(sym);
    const uint64_t id=serial*nshards+index;
    Msg message; message.order=Order{OrderId{id},sym,side,px,qty,now_ns(),tif};validate_order(message.order);
    message.report=std::make_shared<std::promise<ExecutionReport>>();
    Ticket ticket{id,message.report->get_future()};enqueue(index,std::move(message));return ticket;
}
std::future<ExecutionReport> Engine::cancel_async(uint64_t id) {
    if(!id) throw std::invalid_argument("cancel ID must be positive");
    Msg msg;msg.type=MsgType::Cancel;msg.cancel_id=id;msg.order.ts_ns=now_ns();
    msg.report=std::make_shared<std::promise<ExecutionReport>>();auto future=msg.report->get_future();
    enqueue(id%nshards,std::move(msg));return future;
}
std::future<Snapshot> Engine::snapshot(const std::string& sym) {
    validate_symbol(sym); Msg msg;msg.type=MsgType::Snapshot;msg.print_sym=sym;
    msg.snapshot=std::make_shared<std::promise<Snapshot>>();auto future=msg.snapshot->get_future();enqueue(shard_of(sym),std::move(msg));return future;
}
void Engine::submit_print(const std::string& sym) {validate_symbol(sym);Msg msg;msg.type=MsgType::Print;msg.print_sym=sym;enqueue(shard_of(sym),std::move(msg));}
void Engine::flush() {
    std::vector<std::future<void>> barriers;
    for(size_t i=0;i<nshards;++i) {Msg msg;msg.type=MsgType::Barrier;msg.barrier=std::make_shared<std::promise<void>>();barriers.push_back(msg.barrier->get_future());enqueue(i,std::move(msg));}
    for(auto& future:barriers) future.get();check_failure();
}
Snapshot Engine::make_snapshot(const ShardWorker& shard,const std::string& sym) const {
    Snapshot snapshot;snapshot.sym=sym;snapshot.sequence=shard.sequence;snapshot.journal_tip=hex32(shard.journal_tip);
    snapshot.canonical="BOOK1 "+sym+"\n";
    auto found=shard.books.find(sym);
    if(found!=shard.books.end()) {
        const auto& book=found->second; snapshot.active_orders=book.by_id.size();
        auto visit=[&](const auto& levels,auto& depth,char side) {
            for(const auto& [px,level]:levels) {
                depth.push_back({px,level.total_qty,level.q.size()});
                for(const auto* order:level.q) snapshot.canonical+=std::string(1,side)+" "+std::to_string(px)+" "+std::to_string(order->id.v)+" "+std::to_string(order->qty)+"\n";
            }
        };
        visit(book.bids,snapshot.bids,'B');visit(book.asks,snapshot.asks,'S');
    }
    const auto& bytes=snapshot.canonical;snapshot.state_hash=hex32(SHA256::hash(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()));return snapshot;
}
void Engine::apply(ShardWorker& shard,Msg& msg,bool recovering) {
    if(msg.type==MsgType::Barrier) {if(msg.barrier) msg.barrier->set_value();return;}
    if(msg.type==MsgType::Snapshot || msg.type==MsgType::Print) {
        auto value=make_snapshot(shard,msg.print_sym);
        if(msg.snapshot) msg.snapshot->set_value(value);
        if(msg.type==MsgType::Print) emit("{\"event\":\"state_root\",\"sym\":\""+msg.print_sym+"\",\"root\":\""+value.state_hash+"\",\"journal_tip\":\""+value.journal_tip+"\"}");
        return;
    }
    ++shard.sequence;
    ExecutionReport report; report.sequence=shard.sequence;
    const auto original=msg.order;
    if(msg.type==MsgType::New) {
        auto& book=shard.books[msg.order.sym];
        report.id=msg.order.id.v;report.trades=match(book,msg.order,static_cast<long long>(shard.sequence));
        report.remaining=msg.order.qty;report.resting=book.by_id.count(report.id)>0;
        for(const auto& trade:report.trades) if(!book.by_id.count(trade.resting_id.v)) shard.active_symbols.erase(trade.resting_id.v);
        if(report.resting) shard.active_symbols.emplace(report.id,msg.order.sym);
        if(shard.wal && !recovering) {
            const auto raw="N "+std::to_string(original.id.v)+" "+original.sym+" "+std::to_string(static_cast<int>(original.side))+" "+std::to_string(original.px)+" "+std::to_string(original.qty)+" "+std::to_string(static_cast<int>(original.tif));
            shard.wal->append_record(RecType::New,shard.sequence,original.ts_ns,{raw.begin(),raw.end()},report.trades);shard.journal_tip=shard.wal->prev_hash;
        }
        if(!recovering) for(const auto& trade:report.trades) emit("{\"event\":\"trade\",\"seq\":"+std::to_string(trade.seq)+",\"sym\":\""+original.sym+"\",\"px\":"+std::to_string(trade.px)+",\"qty\":"+std::to_string(trade.qty)+"}");
    }else {
        report.id=msg.cancel_id;auto found=shard.active_symbols.find(msg.cancel_id);
        if(found!=shard.active_symbols.end()) {report.cancelled=cancel(shard.books.at(found->second),msg.cancel_id);shard.active_symbols.erase(found);}
        if(shard.wal && !recovering) {const auto raw="C "+std::to_string(msg.cancel_id);shard.wal->append_record(RecType::Cancel,shard.sequence,msg.order.ts_ns,{raw.begin(),raw.end()},{});shard.journal_tip=shard.wal->prev_hash;}
    }
    if(msg.report) msg.report->set_value(std::move(report));
}
void Engine::worker(ShardWorker& shard) {
    Msg msg;
    while(shard.running.load() || !shard.ring.empty()) {
        if(!shard.ring.pop(msg)) {std::this_thread::yield();continue;}
        try {check_failure();apply(shard,msg);}catch(...) {
            auto error=std::current_exception();{std::lock_guard<std::mutex> lock(failure_mutex);if(!failure) failure=error;}
            fail_promises(msg,error);
        }
        msg=Msg{}; // Release completed promises immediately, not when a slot wraps around.
    }
}
