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
        size_t existing=0;
        for(size_t i=0;i<nshards;i++) existing+=std::filesystem::exists(options.wal_directory+"/shard"+std::to_string(i)+".wal");
        if(existing && existing!=nshards) throw std::runtime_error("incomplete WAL shard set; refusing partial recovery");
        for(size_t i=0;i<nshards;++i) {
            auto& shard=*shards[i];
            shard.wal=std::make_unique<WalWriter>(options.wal_directory+"/shard"+std::to_string(i)+".wal",static_cast<uint32_t>(nshards),static_cast<uint32_t>(i),options.sync_wal);
            scan_wal(shard.wal->path,[&](const WalRecord& record) {
                Msg msg;msg.type=record.type==RecType::New?MsgType::New:MsgType::Cancel;
                msg.order=record.order;msg.cancel_id=record.cancel_id;
                const auto id=msg.type==MsgType::New?msg.order.id.v:msg.cancel_id;
                if(id%nshards!=i || (msg.type==MsgType::New && shard_of(msg.order.sym)!=i)) throw std::runtime_error("WAL order routed to wrong shard");
                if(msg.type==MsgType::New) {
                    if(id/nshards>=std::numeric_limits<uint64_t>::max()/nshards-1) throw std::runtime_error("WAL order ID exhausted");
                    next_id.store(std::max(next_id.load(),id/nshards+1));
                }
                apply(shard,msg,true);
            });
            shard.journal_tip=shard.wal->prev_hash;
        }
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
    if(msg.type==MsgType::New) {
        validate_order(msg.order);
        const auto found=shard.books.find(msg.order.sym);
        if(found!=shard.books.end() && (found->second.by_id.count(msg.order.id.v)||found->second.by_id.size()>=1000000))
            throw std::runtime_error("duplicate active ID or active-order capacity exceeded");
    }
    const auto sequence=shard.sequence+1;
    if(shard.wal && !recovering) {
        WalRecord record;record.type=msg.type==MsgType::New?RecType::New:RecType::Cancel;
        record.seq=sequence;record.ts_ns=static_cast<uint64_t>(msg.order.ts_ns);record.order=msg.order;record.cancel_id=msg.cancel_id;
        shard.wal->append(record); // Successful append+optional fsync BEFORE any book mutation.
        shard.journal_tip=shard.wal->prev_hash;
    }
    shard.sequence=sequence;
    ExecutionReport report; report.sequence=shard.sequence;
    const auto original=msg.order;
    if(msg.type==MsgType::New) {
        auto& book=shard.books[msg.order.sym];
        report.id=msg.order.id.v;report.trades=match(book,msg.order,static_cast<long long>(shard.sequence));
        report.remaining=msg.order.qty;report.resting=book.by_id.count(report.id)>0;
        for(const auto& trade:report.trades) if(!book.by_id.count(trade.resting_id.v)) shard.active_symbols.erase(trade.resting_id.v);
        if(report.resting) shard.active_symbols.emplace(report.id,msg.order.sym);
        if(!recovering) for(const auto& trade:report.trades) emit("{\"event\":\"trade\",\"seq\":"+std::to_string(trade.seq)+",\"shard\":"+std::to_string(original.id.v%nshards)+",\"resting_id\":"+std::to_string(trade.resting_id.v)+",\"incoming_id\":"+std::to_string(trade.incoming_id.v)+",\"sym\":\""+original.sym+"\",\"px\":"+std::to_string(trade.px)+",\"qty\":"+std::to_string(trade.qty)+"}");
    }else {
        report.id=msg.cancel_id;auto found=shard.active_symbols.find(msg.cancel_id);
        if(found!=shard.active_symbols.end()) {report.cancelled=cancel(shard.books.at(found->second),msg.cancel_id);shard.active_symbols.erase(found);}

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
