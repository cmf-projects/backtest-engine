#include "population/file_book.hpp"

#include <stdexcept>
#include <utility>

namespace population {

FileBook::FileBook(TapePtr tape) : tape_(std::move(tape)) {
    if (!tape_) {
        throw std::invalid_argument("population::FileBook: null tape");
    }
}

void FileBook::Start(Clock& clock) {
    if (started_) {
        throw std::logic_error("population::FileBook: already started");
    }
    auto first = tape_->Next();
    if (!first) {
        throw std::runtime_error("population::FileBook: empty tape on Start");
    }
    clock.Start(first->ts_ns);
    current_ = std::move(*first);
    started_ = true;
}

bool FileBook::Advance(Clock& clock) {
    if (!started_) {
        throw std::logic_error("population::FileBook: Advance before Start");
    }
    if (ended_) {
        return false;
    }
    auto next = tape_->Next();
    if (!next) {
        ended_ = true;
        return false;
    }
    clock.AdvanceTo(next->ts_ns);
    current_ = std::move(*next);
    return true;
}

const SnapshotEvent& FileBook::current() const {
    if (!current_) {
        throw std::logic_error("population::FileBook: no current snapshot");
    }
    return *current_;
}

}  // namespace population
