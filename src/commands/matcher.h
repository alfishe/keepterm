// The fuzzy matcher: fzf's FuzzyMatchV2 scoring as Macterm uses it.
//
// One matcher for every search in the app: the palette and its scopes, Settings search,
// Keymaps, the Resources tree filter. Higher scores are better.
//
//   - prepare(): fold case and diacritics once (combining marks fold away), per-scalar
//     bonuses from the unfolded text, a 64-bit presence mask, byte offsets for highlights;
//   - score(): mask AND, a greedy forward scan that also bounds the window a match can
//     occupy, a two-row DP over that window with reused scratch buffers (no allocation per
//     candidate);
//   - positions(): the full matrix walked back, for the visible rows only;
//   - SearchIndex: multi-term queries over records of fields, field penalty, top-K, parallel
//     above 20,000 records; SearchSession: narrowing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kt::commands::palette {

inline constexpr std::int32_t kScoreMatch = 16;
inline constexpr std::int32_t kScoreGapStart = -3;
inline constexpr std::int32_t kScoreGapExtension = -1;
inline constexpr std::int16_t kBonusBoundary = 8;        // after other non-word characters
inline constexpr std::int16_t kBonusNonWord = 8;         // a non-word character itself
inline constexpr std::int16_t kBonusCamel = 7;           // camelCase hump, letter → digit
inline constexpr std::int32_t kBonusConsecutive = 4;
inline constexpr std::int32_t kFirstCharMultiplier = 2;
inline constexpr std::int16_t kBonusBoundaryWhite = 10;  // after whitespace (and at the start)
inline constexpr std::int16_t kBonusBoundaryDelimiter = 9;   // after / \ : ; , . _ - |
inline constexpr std::int32_t kNoScore = INT32_MIN / 4;
inline constexpr std::int32_t kFieldPenalty = 16;

// Prepared text: folded scalars, bonuses, mask, offsets back to the source.
struct SearchText {
    std::vector<std::uint32_t> scalars;   // folded
    std::vector<std::int16_t> bonus;      // per folded scalar
    std::vector<std::uint32_t> offsets;   // per folded scalar: byte offset of its source character
    std::uint64_t mask = 0;
    std::uint32_t length = 0;             // source length in scalars (tie-break: shorter wins)
};
SearchText prepare(std::string_view utf8);

// One whitespace-separated word of a query, folded like SearchText.
struct SearchTerm {
    std::vector<std::uint32_t> scalars;
    std::uint64_t mask = 0;
};
SearchTerm make_term(std::string_view word);

struct SearchQuery {
    std::vector<SearchTerm> terms;
    static SearchQuery parse(std::string_view text);
    bool empty() const { return terms.empty(); }
    // Everything `this` matches is among what `previous` matched: each earlier term only grew
    // at its end and new terms only narrow.
    bool narrows(const SearchQuery& previous) const;
};

// Buffers score() reuses, grown to the largest candidate seen. One per thread.
class Scratch {
public:
    void reserve(std::size_t rows, std::size_t columns);
    std::int32_t* first() { return first_.data(); }
    std::int32_t* buf(int i) { return bufs_[static_cast<std::size_t>(i)].data(); }

private:
    std::vector<std::int32_t> first_;
    std::vector<std::int32_t> bufs_[6];
};

// The best score of `t` as a subsequence of `s`, or nullopt. An empty term scores 0.
std::optional<std::int32_t> score(const SearchTerm& t, const SearchText& s, Scratch& scratch);
// Indices into s.scalars of the best alignment, or nullopt when there is no match.
std::optional<std::vector<std::uint32_t>> positions(const SearchTerm& t, const SearchText& s);

// Every term must match some field; a match in field i costs kFieldPenalty * i.
std::optional<std::int32_t> score_fields(const SearchQuery& q, std::span<const SearchText> fields, Scratch& scratch);
std::optional<std::int32_t> score_fields(const SearchQuery& q, std::span<const SearchText* const> fields, Scratch& scratch);
// Byte offsets in the title (source characters) that the terms matched, sorted, unique.
std::vector<std::uint32_t> highlights(const SearchQuery& q, const SearchText& title);
// `text` starts with `prefix`, both folded (path completion).
bool folded_has_prefix(std::string_view text, std::string_view prefix);
// The folded scalars of a string as UTF-8 (tests, diagnostics).
std::string fold(std::string_view utf8);

struct Match {
    std::uint32_t index = 0;
    std::int32_t score = 0;
    friend bool operator==(const Match&, const Match&) = default;
};

class SearchIndex {
public:
    static constexpr std::size_t kParallelThreshold = 20000;

    explicit SearchIndex(const std::vector<std::vector<std::string>>& records);   // fields per record
    explicit SearchIndex(std::vector<std::vector<SearchText>> prepared);

    std::size_t size() const { return records_.size(); }
    const std::vector<SearchText>& record(std::size_t i) const { return records_[i]; }

    // Unordered matches, in parallel above kParallelThreshold. `among` restricts the search.
    std::vector<Match> match_all(const SearchQuery& q, const std::vector<std::uint32_t>* among = nullptr) const;
    // Best first: score, then shorter first field, then index. limit 0 = all; otherwise a
    // bounded heap keeps the best `limit`.
    std::vector<Match> rank(std::vector<Match> matches, std::size_t limit = 0) const;
    std::vector<Match> search(const SearchQuery& q, std::size_t limit = 0,
                              const std::vector<std::uint32_t>* among = nullptr) const;
    // Serial reference path (tests compare the parallel one to it).
    std::vector<Match> match_serial(const SearchQuery& q) const;

private:
    std::vector<std::vector<SearchText>> records_;
};

// A SearchIndex searched keystroke by keystroke; narrowing reuses the last matches.
class SearchSession {
public:
    explicit SearchSession(const SearchIndex& index) : index_(&index) {}
    std::vector<Match> search(std::string_view text, std::size_t limit = 0);
    bool last_was_narrowed() const { return narrowed_; }

private:
    const SearchIndex* index_;
    std::optional<SearchQuery> last_;
    std::vector<std::uint32_t> last_matches_;
    bool narrowed_ = false;
};

// Runs fn(chunk) for chunk in [0, chunks) on a shared pool of hardware_concurrency - 1
// threads plus the caller. Used above the parallel threshold.
void parallel_for(std::size_t chunks, const std::function<void(std::size_t)>& fn);

}  // namespace kt::commands::palette
