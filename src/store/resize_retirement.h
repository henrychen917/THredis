// A resize reserves its retirement record before publishing a second table. Completion can
// therefore unlink the old table without allocation or a wait for the object-retirement ring.
// Records follow their store until handoff, then belong only to the retiring owner's QSBR FIFO.
#pragma once
#include <cstdint>
#include <cstdlib>

namespace tomo {

struct ResizeRetirement {
    uint64_t* table = nullptr;
    uint64_t stamp = 0;
    ResizeRetirement* next = nullptr;
    ~ResizeRetirement() { std::free(table); }
};

class ResizeRetireQueue {
public:
    ~ResizeRetireQueue() { drain_all(); }
    bool empty() const { return head_ == nullptr; }
    uint32_t size() const { return count_; }
    uint64_t head_stamp() const { return head_->stamp; }

    void push(ResizeRetirement* record, uint64_t* table, uint64_t stamp) {
        record->table = table;
        record->stamp = stamp;
        record->next = nullptr;
        if (tail_) tail_->next = record;
        else head_ = record;
        tail_ = record;
        count_++;
    }

    uint32_t drain_below(uint64_t floor) {
        uint32_t drained = 0;
        while (head_ && head_->stamp < floor) {
            pop();
            drained++;
        }
        return drained;
    }
    uint32_t drain_all() {
        const uint32_t drained = count_;
        while (head_) pop();
        return drained;
    }

private:
    void pop() {
        ResizeRetirement* retired = head_;
        head_ = retired->next;
        if (!head_) tail_ = nullptr;
        count_--;
        delete retired;
    }
    ResizeRetirement* head_ = nullptr;
    ResizeRetirement* tail_ = nullptr;
    uint32_t count_ = 0;
};

} // namespace tomo
