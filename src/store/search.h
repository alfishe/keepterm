// Search across sessions.
//
//   1. the catalog selects segments by kind, time, session and project, newest first;
//   2. per segment, the trigram index gives candidate blocks (literal: AND of the needle's
//      trigrams; regex: the required-trigram query of store/regex_query.h); unsealed segments
//      and unindexed kinds are scanned in full;
//   3. candidate blocks are decompressed and every record is verified (folded literal, exact
//      literal, or PCRE2);
//   4. hits are streamed newest first: segments in catalog order, blocks and records inside a
//      segment from the newest. Segments run in parallel on cores - 1 threads; the results of
//      a segment are handed out only after those of every newer segment.
//
// on_hit runs on the calling thread and returns false to stop. `cancel` is polled between
// blocks.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "store/catalog.h"

namespace kt::store {

struct SearchQuery {
    std::string pattern;
    bool regex = false;
    bool case_sensitive = false;
    std::optional<uint64_t> since_ms, until_ms;
    std::vector<std::string> sessions;
    std::optional<std::string> project;
    std::vector<Kind> kinds{Kind::text};
    std::size_t limit = 100;
    unsigned threads = 0;        // 0: hardware threads - 1, at least 1
    std::size_t max_text = 512;  // bytes of the line kept in a hit
};

struct Hit {
    std::string session, project, cwd;
    Kind kind = Kind::text;
    uint64_t ts_ms = 0, seq = 0;
    std::string text;            // the matching line (cut at max_text)
    std::size_t match_offset = 0, match_len = 0;
    int64_t segment_id = 0;
    std::string file;
    uint32_t block = 0, record = 0;  // jump target inside the segment
};

struct SearchStats {
    uint32_t segments = 0, segments_full_scan = 0, segments_missing = 0;
    uint64_t blocks_total = 0, blocks_candidate = 0, blocks_read = 0, blocks_corrupt = 0;
    uint64_t raw_bytes_scanned = 0;
    uint64_t hits = 0;
    bool cancelled = false, limit_reached = false;
    double first_hit_ms = -1, total_ms = 0;
    std::string plan;  // the trigram query, for --explain
};

// Compiles the matcher and the trigram plan without running (syntax check, --explain).
Result<std::string> explain(const SearchQuery& q);

Result<SearchStats> search(Catalog& catalog, const std::filesystem::path& root, const SearchQuery& q,
                           const std::function<bool(const Hit&)>& on_hit, const std::atomic<bool>* cancel = nullptr);

}  // namespace kt::store
