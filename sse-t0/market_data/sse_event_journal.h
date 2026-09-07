#ifndef SSE_PIPELINE_EVENT_JOURNAL_H
#define SSE_PIPELINE_EVENT_JOURNAL_H

#include "sse_event.h"
#include "../../modules/deepwin_guoxin/md/SZERecoverable.h"

#include <cstdint>
#include <string>

namespace sse_pipeline {

struct JournalOptions {
    JournalOptions();

    std::string directory;
    std::uint32_t trading_day;
    std::uint64_t segment_bytes;
    std::uint64_t min_free_bytes;
};

class EventJournal {
public:
    EventJournal();
    ~EventJournal();

    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;

    bool open(const JournalOptions& options, std::string* error);
    bool append(const Event& event, std::string* error);
    bool flush(std::string* error);
    bool close(bool clean_shutdown, std::string* error);

    bool is_open() const { return open_; }

private:
    sze_recovery::JournalConfig config_for(const JournalOptions& options) const;

    sze_recovery::JournalWriter writer_;
    JournalOptions options_;
    std::uint64_t next_event_id_;
    bool open_;
};

class EventJournalReader {
public:
    enum NextResult {
        Record = 0,
        End = 1,
        Error = 2
    };

    EventJournalReader();
    ~EventJournalReader();

    EventJournalReader(const EventJournalReader&) = delete;
    EventJournalReader& operator=(const EventJournalReader&) = delete;

    bool open(const JournalOptions& options, std::string* error);
    NextResult next(Event* event, std::string* error);
    bool close();

    bool is_open() const { return open_; }

private:
    sze_recovery::JournalConfig config_for(const JournalOptions& options) const;

    sze_recovery::JournalReader reader_;
    JournalOptions options_;
    std::uint64_t next_event_id_;
    bool open_;
    bool failed_;
};

}  // namespace sse_pipeline

#endif  // SSE_PIPELINE_EVENT_JOURNAL_H
