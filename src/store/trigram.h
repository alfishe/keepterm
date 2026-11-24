// Trigram index of the text and command kinds.
//
// A trigram is three consecutive bytes of case-folded UTF-8 (store/fold.h), as a 24-bit key.
// Posting lists map a trigram to the blocks of one segment that contain it. On disk (the
// footer of a sealed segment):
//
//   meta part (read when the segment opens, covered by the trailer's meta CRC):
//     u32 ntrigrams, u32 nchunks, nchunks x {u32 first_key, u32 offset, u32 len, u32 crc32c}
//   postings part (read per query, one chunk at a time, offsets relative to its start):
//     chunks of up to kChunkKeys entries: varint key_delta, varint (len << 1 | bitmap), bytes
//     where bytes are block ids, delta + varint coded (the first one absolute), or a bitmap of
//     ceil(nblocks / 8) bytes, whichever is shorter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::store::trigram {

using Key = uint32_t;
inline constexpr std::size_t kChunkKeys = 256;

constexpr Key make_key(unsigned char a, unsigned char b, unsigned char c) {
    return (Key{a} << 16) | (Key{b} << 8) | Key{c};
}

// A query over posting lists. and_: every trigram and every sub must hold. or_: any of them.
// all: no restriction (every block is a candidate). none: no block can match.
struct Query {
    enum class Op : uint8_t { all, none, and_, or_ };
    Op op = Op::all;
    std::vector<Key> trigrams;
    std::vector<Query> subs;

    static Query all_blocks() { return {}; }
    static Query no_blocks() { return {Op::none, {}, {}}; }
    bool is_all() const { return op == Op::all; }
};
// AND of the trigrams of one folded string; all when it is shorter than 3 bytes.
Query literal_query(std::string_view folded);
Query and_of(Query a, Query b);
Query or_of(Query a, Query b);
std::string describe(const Query&);  // for tests and --explain
void collect_keys(const Query&, std::vector<Key>& out);

// A set of blocks as a bitmap.
struct Bits {
    std::vector<uint64_t> w;
    std::size_t n = 0;
    explicit Bits(std::size_t count = 0, bool on = false);
    void set(std::size_t i) { w[i >> 6] |= uint64_t{1} << (i & 63); }
    bool test(std::size_t i) const { return (w[i >> 6] >> (i & 63)) & 1u; }
    void and_with(const Bits& o);
    void or_with(const Bits& o);
    std::size_t count() const;
};

// Evaluates `q` given the posting bits of each trigram (`lookup` returns them for one key).
Bits evaluate(const Query& q, std::size_t nblocks, const std::function<const Bits&(Key)>& lookup);

// Collects the distinct trigrams of the folded text of one block. One builder per writer: it
// owns a 2 MiB bitmap that is cleared incrementally.
class BlockSetBuilder {
public:
    BlockSetBuilder();
    void add(std::string_view folded);      // trigrams inside `folded` (not across calls)
    std::vector<Key> take_sorted();          // the set so far, sorted; resets the builder

private:
    std::vector<uint64_t> seen_;     // one bit per key (2 MiB)
    std::vector<uint64_t> summary_;  // one bit per word of seen_ (32 KiB): sorted output without a sort
    std::size_t count_ = 0;
};

// The index of an open segment: one delta-coded sorted key set per block. Serializes to the
// footer form at seal.
class MemoryIndex {
public:
    void add_block(const std::vector<Key>& sorted_keys);
    std::size_t blocks() const { return sets_.size(); }
    std::size_t bytes() const;
    Bits candidates(const Query& q) const;
    // Footer parts: `meta` gets ntrigrams, nchunks and the chunk table; `postings` the chunks.
    void serialize(std::string& meta, std::string& postings) const;
    void clear() { sets_.clear(); }

private:
    std::vector<std::string> sets_;
};

// The index of a sealed segment. Parses the chunk table; postings are fetched on demand.
class SealedIndex {
public:
    // `meta` starts at the ntrigrams field; advances over the index meta part.
    bool parse(const char*& p, const char* end, std::size_t nblocks);
    std::size_t trigrams() const { return ntrigrams_; }
    std::size_t chunks() const { return chunks_.size(); }
    // Reads `len` bytes at `offset` of the postings part into `out`.
    using ReadFn = std::function<Result<void>(uint64_t offset, uint32_t len, std::string& out)>;
    Result<Bits> candidates(const Query& q, const ReadFn& read) const;
    // Every (key, blocks) pair, for tests and compaction checks.
    Result<void> for_each(const ReadFn& read, const std::function<void(Key, const std::vector<uint32_t>&)>& fn) const;

private:
    struct Chunk {
        Key first_key;
        uint32_t offset, len, crc;
    };
    Result<void> load_chunk(std::size_t i, const ReadFn& read, std::string& buf) const;
    std::size_t nblocks_ = 0;
    std::size_t ntrigrams_ = 0;
    std::vector<Chunk> chunks_;
};

// Decodes one posting value (the bytes after the len|bitmap varint).
bool decode_posting(std::string_view bytes, bool bitmap, std::size_t nblocks, Bits& out);

}  // namespace kt::store::trigram
