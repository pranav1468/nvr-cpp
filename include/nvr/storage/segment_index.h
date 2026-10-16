#pragma once

#include "nvr/common/types.h"
#include <vector>
#include <optional>

namespace nvr {

struct ReconciliationReport {
    size_t ghost_records_pruned{0};
    size_t unindexed_files_recovered{0};
    size_t corrupt_files_removed{0};
};

class SegmentIndex {
public:
    static SegmentIndex& Instance();

    bool InsertSegment(const SegmentMetadata& meta);

    std::vector<SegmentMetadata> QuerySegments(int channel_id, int64_t start_ms, int64_t end_ms);

    std::vector<SegmentMetadata> GetOldestUnlockedSegments(int limit);

    bool DeleteSegmentRecord(int64_t id);

    bool LockSegment(int64_t id, bool is_locked);

    uint64_t GetTotalRecordingSizeBytes();

    int GetTotalSegmentCount();

    ReconciliationReport ReconcileStorage(const std::string& recording_path);

private:
    SegmentIndex() = default;
    ~SegmentIndex() = default;
    SegmentIndex(const SegmentIndex&) = delete;
    SegmentIndex& operator=(const SegmentIndex&) = delete;
};

} // namespace nvr
