#include "engine.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: wal_replay DIRECTORY SHARDS SYMBOL\n";
        return 1;
    }
    try {
        const size_t count = std::stoul(argv[2]);
        for (size_t i = 0; i < count; i++)
            if (!std::filesystem::exists(std::string(argv[1]) + "/shard" + std::to_string(i) +
                                         ".wal"))
                throw std::runtime_error("missing replay shard file");
        Engine engine(count, EngineOptions{argv[1], true});
        engine.start();
        const auto snapshot = engine.snapshot(argv[3]).get();
        engine.stop();
        std::cout << snapshot.canonical << "STATE_HASH " << snapshot.state_hash << "\nJOURNAL_TIP "
                  << snapshot.journal_tip << "\nSEQUENCE " << snapshot.sequence << '\n';
    } catch (const std::exception &error) {
        std::cerr << "REPLAY FAILED " << error.what() << '\n';
        return 2;
    }
}
