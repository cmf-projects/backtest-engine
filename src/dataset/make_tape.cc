#include "dataset/make_tape.hpp"

#include "dataset/feather_tape.hpp"
#include "dataset/jsonl_tape.hpp"

#include <filesystem>

namespace dataset {

population::TapePtr MakeTape(const std::string& path, const std::string& symbol) {
    const std::filesystem::path p(path);
    if (p.extension() == ".feather" || std::filesystem::is_directory(p)) {
        return MakeFeatherBboTape(path, symbol);
    }
    return MakeJsonlTape(path, symbol);
}

}  // namespace dataset
