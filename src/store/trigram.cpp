#include "store/trigram.h"

#include <algorithm>
#include <bit>
#include <map>
#include <span>

#include "base/crc32c.h"
#include "store/format.h"

namespace kt::store::trigram {

namespace {

constexpr std::size_t kMaxLiteralKeys = 16;

std::string key_text(Key k) {
    std::string s;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const auto c = static_cast<unsigned char>((k >> sh) & 0xFFu);
        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
            s.push_back(static_cast<char>(c));
        } else {
            static constexpr char hex[] = "0123456789abcdef";
            s += "\\x";
            s.push_back(hex[c >> 4]);
            s.push_back(hex[c & 15]);
        }
    }
    return s;
}

void sort_unique(std::vector<Key>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Encodes the block ids of one trigram: delta varints or a bitmap, whichever is shorter.
void encode_posting(std::span<const uint32_t> ids, std::size_t nblocks, std::string& out) {
    thread_local std::string list;
    list.clear();
    uint32_t prev = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        put_varint(list, i == 0 ? ids[i] : ids[i] - prev);
        prev = ids[i];
    }
    const std::size_t bitmap_len = (nblocks + 7) / 8;
    if (bitmap_len < list.size()) {
        std::string bm(bitmap_len, '\0');
        for (const uint32_t id : ids) bm[id >> 3] = static_cast<char>(static_cast<unsigned char>(bm[id >> 3]) | (1u << (id & 7)));
        put_varint(out, (uint64_t{bitmap_len} << 1) | 1u);
        out += bm;
    } else {
        put_varint(out, uint64_t{list.size()} << 1);
        out += list;
    }
}

}  // namespace

Query literal_query(std::string_view s) {
    if (s.size() < 3) return Query::all_blocks();
    Query q{Query::Op::and_, {}, {}};
    for (std::size_t i = 0; i + 3 <= s.size(); ++i) {
        q.trigrams.push_back(make_key(static_cast<unsigned char>(s[i]), static_cast<unsigned char>(s[i + 1]),
                                      static_cast<unsigned char>(s[i + 2])));
    }
    sort_unique(q.trigrams);
    if (q.trigrams.size() > kMaxLiteralKeys) {
        // Every trigram is required; a spread subset keeps the lookups bounded, verification
        // rejects the extra candidates.
        std::vector<Key> pick;
        for (std::size_t i = 0; i < kMaxLiteralKeys; ++i) pick.push_back(q.trigrams[i * q.trigrams.size() / kMaxLiteralKeys]);
        q.trigrams = std::move(pick);
    }
    return q;
}

Query and_of(Query a, Query b) {
    using Op = Query::Op;
    if (a.op == Op::none || b.op == Op::none) return Query::no_blocks();
    if (a.op == Op::all) return b;
    if (b.op == Op::all) return a;
    Query out{Op::and_, {}, {}};
    for (Query* q : {&a, &b}) {
        if (q->op == Op::and_) {
            out.trigrams.insert(out.trigrams.end(), q->trigrams.begin(), q->trigrams.end());
            for (auto& s : q->subs) out.subs.push_back(std::move(s));
        } else {
            out.subs.push_back(std::move(*q));
        }
    }
    sort_unique(out.trigrams);
    return out;
}

Query or_of(Query a, Query b) {
    using Op = Query::Op;
    if (a.op == Op::all || b.op == Op::all) return Query::all_blocks();
    if (a.op == Op::none) return b;
    if (b.op == Op::none) return a;
    Query out{Op::or_, {}, {}};
    for (Query* q : {&a, &b}) {
        if (q->op == Op::or_) {
            out.trigrams.insert(out.trigrams.end(), q->trigrams.begin(), q->trigrams.end());
            for (auto& s : q->subs) out.subs.push_back(std::move(s));
        } else if (q->op == Op::and_ && q->subs.empty() && q->trigrams.size() == 1) {
            out.trigrams.push_back(q->trigrams[0]);
        } else {
            out.subs.push_back(std::move(*q));
        }
    }
    sort_unique(out.trigrams);
    return out;
}

std::string describe(const Query& q) {
    switch (q.op) {
        case Query::Op::all: return "+";
        case Query::Op::none: return "-";
        default: break;
    }
    std::string s = "(";
    const char* sep = q.op == Query::Op::and_ ? " " : " | ";
    bool first = true;
    for (const Key k : q.trigrams) {
        if (!first) s += sep;
        first = false;
        s += "\"" + key_text(k) + "\"";
    }
    for (const auto& sub : q.subs) {
        if (!first) s += sep;
        first = false;
        s += describe(sub);
    }
    return s + ")";
}

void collect_keys(const Query& q, std::vector<Key>& out) {
    out.insert(out.end(), q.trigrams.begin(), q.trigrams.end());
    for (const auto& s : q.subs) collect_keys(s, out);
}

Bits::Bits(std::size_t count, bool on) : w((count + 63) / 64, on ? ~uint64_t{0} : 0), n(count) {
    if (on && (count & 63)) w.back() = (uint64_t{1} << (count & 63)) - 1;
}

void Bits::and_with(const Bits& o) {
    for (std::size_t i = 0; i < w.size(); ++i) w[i] &= i < o.w.size() ? o.w[i] : 0;
}
void Bits::or_with(const Bits& o) {
    for (std::size_t i = 0; i < w.size() && i < o.w.size(); ++i) w[i] |= o.w[i];
}
std::size_t Bits::count() const {
    std::size_t c = 0;
    for (const uint64_t x : w) c += static_cast<std::size_t>(std::popcount(x));
    return c;
}

Bits evaluate(const Query& q, std::size_t nblocks, const std::function<const Bits&(Key)>& lookup) {
    switch (q.op) {
        case Query::Op::all: return Bits(nblocks, true);
        case Query::Op::none: return Bits(nblocks, false);
        case Query::Op::and_: {
            Bits b(nblocks, true);
            for (const Key k : q.trigrams) b.and_with(lookup(k));
            for (const auto& s : q.subs) b.and_with(evaluate(s, nblocks, lookup));
            return b;
        }
        case Query::Op::or_: {
            Bits b(nblocks, false);
            for (const Key k : q.trigrams) b.or_with(lookup(k));
            for (const auto& s : q.subs) b.or_with(evaluate(s, nblocks, lookup));
            return b;
        }
    }
    return Bits(nblocks, true);
}

BlockSetBuilder::BlockSetBuilder() : seen_((1u << 24) / 64, 0), summary_((1u << 24) / 64 / 64, 0) {}

void BlockSetBuilder::add(std::string_view s) {
    if (s.size() < 3) return;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    Key k = (Key{p[0]} << 8) | p[1];
    for (std::size_t i = 2; i < s.size(); ++i) {
        k = ((k << 8) | p[i]) & 0xFFFFFFu;
        uint64_t& word = seen_[k >> 6];
        const uint64_t bit = uint64_t{1} << (k & 63);
        if (!(word & bit)) {
            if (word == 0) summary_[k >> 12] |= uint64_t{1} << ((k >> 6) & 63);
            word |= bit;
            ++count_;
        }
    }
}

std::vector<Key> BlockSetBuilder::take_sorted() {
    std::vector<Key> out;
    out.reserve(count_);
    for (std::size_t si = 0; si < summary_.size(); ++si) {
        uint64_t sw = summary_[si];
        if (!sw) continue;
        summary_[si] = 0;
        while (sw) {
            const auto wi = (si << 6) | static_cast<std::size_t>(std::countr_zero(sw));
            sw &= sw - 1;
            uint64_t w = seen_[wi];
            seen_[wi] = 0;
            while (w) {
                out.push_back(static_cast<Key>((wi << 6) | static_cast<std::size_t>(std::countr_zero(w))));
                w &= w - 1;
            }
        }
    }
    count_ = 0;
    return out;
}

void MemoryIndex::add_block(const std::vector<Key>& keys) {
    std::string s;
    Key prev = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        put_varint(s, i == 0 ? keys[i] : keys[i] - prev);
        prev = keys[i];
    }
    s.shrink_to_fit();
    sets_.push_back(std::move(s));
}

std::size_t MemoryIndex::bytes() const {
    std::size_t n = 0;
    for (const auto& s : sets_) n += s.size();
    return n;
}

namespace {

// Walks one delta-coded key set.
struct SetCursor {
    const char* p = nullptr;
    const char* end = nullptr;
    Key key = 0;
    bool valid = false;
    bool started = false;

    explicit SetCursor(const std::string& s) : p(s.data()), end(s.data() + s.size()) { next(); }
    void next() {
        uint64_t v = 0;
        if (p >= end || !get_varint(p, end, v)) {
            valid = false;
            return;
        }
        key = started ? key + static_cast<Key>(v) : static_cast<Key>(v);
        started = true;
        valid = true;
    }
};

}  // namespace

Bits MemoryIndex::candidates(const Query& q) const {
    const std::size_t nb = sets_.size();
    if (q.is_all()) return Bits(nb, true);
    std::vector<Key> keys;
    collect_keys(q, keys);
    sort_unique(keys);
    std::map<Key, Bits> postings;
    for (const Key k : keys) postings.emplace(k, Bits(nb));
    for (std::size_t b = 0; b < nb; ++b) {
        SetCursor c(sets_[b]);
        std::size_t i = 0;
        while (c.valid && i < keys.size()) {
            if (c.key < keys[i]) {
                c.next();
            } else if (c.key > keys[i]) {
                ++i;
            } else {
                postings[keys[i]].set(b);
                ++i;
                c.next();
            }
        }
    }
    return evaluate(q, nb, [&](Key k) -> const Bits& { return postings.at(k); });
}

void MemoryIndex::serialize(std::string& meta, std::string& postings) const {
    const std::size_t nb = sets_.size();
    std::vector<SetCursor> cur;
    cur.reserve(nb);
    for (const auto& s : sets_) cur.emplace_back(s);

    struct ChunkOut {
        Key first_key;
        uint32_t offset, len;
    };
    std::vector<ChunkOut> chunks;
    uint32_t ntrigrams = 0;
    std::size_t in_chunk = 0;
    Key prev_key = 0;
    std::vector<uint32_t> pairs;  // block ids of the bucket's keys, with the keys' low 16 bits in `lows`
    std::vector<uint16_t> lows;
    std::vector<uint32_t> start(65537), ids, sorted;
    const std::size_t base = postings.size();

    auto emit = [&](Key key, std::span<const uint32_t> blocks) {
        if (in_chunk == 0 || in_chunk == kChunkKeys) {
            if (!chunks.empty()) chunks.back().len = static_cast<uint32_t>(postings.size() - base - chunks.back().offset);
            chunks.push_back({key, static_cast<uint32_t>(postings.size() - base), 0});
            in_chunk = 0;
            prev_key = key;
        }
        put_varint(postings, key - prev_key);
        prev_key = key;
        encode_posting(blocks, nb, postings);
        ++in_chunk;
        ++ntrigrams;
    };

    // Bucket by the first byte of the trigram: each block's keys are sorted, so a cursor per
    // block yields the keys of one bucket at a time and memory stays at one bucket. Inside a
    // bucket a counting sort on the low 16 bits keeps the block ids of a key ascending.
    for (uint32_t bucket = 0; bucket < 256; ++bucket) {
        pairs.clear();
        lows.clear();
        for (std::size_t b = 0; b < nb; ++b) {
            auto& c = cur[b];
            while (c.valid && (c.key >> 16) == bucket) {
                pairs.push_back(static_cast<uint32_t>(b));
                lows.push_back(static_cast<uint16_t>(c.key & 0xFFFFu));
                c.next();
            }
        }
        if (pairs.empty()) continue;
        std::fill(start.begin(), start.end(), 0u);
        for (const uint16_t l : lows) ++start[l + 1u];
        for (std::size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
        sorted.resize(pairs.size());
        ids.assign(start.begin(), start.end() - 1);  // fill positions
        for (std::size_t i = 0; i < pairs.size(); ++i) sorted[ids[lows[i]]++] = pairs[i];
        for (uint32_t l = 0; l < 65536; ++l) {
            if (start[l] == start[l + 1]) continue;
            emit((bucket << 16) | l, std::span<const uint32_t>(sorted.data() + start[l], start[l + 1] - start[l]));
        }
    }
    if (!chunks.empty()) chunks.back().len = static_cast<uint32_t>(postings.size() - base - chunks.back().offset);

    put_u32(meta, ntrigrams);
    put_u32(meta, static_cast<uint32_t>(chunks.size()));
    for (const auto& c : chunks) {
        put_u32(meta, c.first_key);
        put_u32(meta, c.offset);
        put_u32(meta, c.len);
        put_u32(meta, crc32c(std::as_bytes(std::span(postings.data() + base + c.offset, c.len))));
    }
}

bool decode_posting(std::string_view bytes, bool bitmap, std::size_t nblocks, Bits& out) {
    if (bitmap) {
        if (bytes.size() != (nblocks + 7) / 8) return false;
        for (std::size_t i = 0; i < nblocks; ++i) {
            if (static_cast<unsigned char>(bytes[i >> 3]) & (1u << (i & 7))) out.set(i);
        }
        return true;
    }
    const char* p = bytes.data();
    const char* end = p + bytes.size();
    uint64_t id = 0;
    bool first = true;
    while (p < end) {
        uint64_t v = 0;
        if (!get_varint(p, end, v)) return false;
        id = first ? v : id + v;
        first = false;
        if (id >= nblocks) return false;
        out.set(static_cast<std::size_t>(id));
    }
    return true;
}

bool SealedIndex::parse(const char*& p, const char* end, std::size_t nblocks) {
    nblocks_ = nblocks;
    if (end - p < 8) return false;
    ntrigrams_ = load_u32(reinterpret_cast<const std::byte*>(p));
    const uint32_t nchunks = load_u32(reinterpret_cast<const std::byte*>(p + 4));
    p += 8;
    if (static_cast<std::size_t>(end - p) < std::size_t{nchunks} * 16) return false;
    chunks_.clear();
    chunks_.reserve(nchunks);
    for (uint32_t i = 0; i < nchunks; ++i) {
        const auto* b = reinterpret_cast<const std::byte*>(p);
        chunks_.push_back({load_u32(b), load_u32(b + 4), load_u32(b + 8), load_u32(b + 12)});
        p += 16;
        if (i > 0 && chunks_[i].first_key <= chunks_[i - 1].first_key) return false;
    }
    return true;
}

Result<void> SealedIndex::load_chunk(std::size_t i, const ReadFn& read, std::string& buf) const {
    const Chunk& c = chunks_[i];
    auto r = read(c.offset, c.len, buf);
    if (!r) return r;
    if (crc32c(std::as_bytes(std::span(buf.data(), buf.size()))) != c.crc) {
        return make_error(Errc::corrupt, "index chunk checksum mismatch");
    }
    return {};
}

namespace {

// Visits the entries of one loaded chunk; `fn` returns false to stop.
template <class Fn>
bool walk_chunk(const std::string& buf, Key first_key, Fn&& fn) {
    const char* p = buf.data();
    const char* end = p + buf.size();
    Key key = first_key;
    bool first = true;
    while (p < end) {
        uint64_t d = 0, lb = 0;
        if (!get_varint(p, end, d) || !get_varint(p, end, lb)) return false;
        key = first ? first_key + static_cast<Key>(d) : key + static_cast<Key>(d);
        first = false;
        const uint64_t len = lb >> 1;
        if (len > static_cast<uint64_t>(end - p)) return false;
        if (!fn(key, std::string_view(p, static_cast<std::size_t>(len)), (lb & 1u) != 0)) return true;
        p += len;
    }
    return true;
}

}  // namespace

Result<Bits> SealedIndex::candidates(const Query& q, const ReadFn& read) const {
    if (q.is_all()) return Bits(nblocks_, true);
    std::vector<Key> keys;
    collect_keys(q, keys);
    sort_unique(keys);
    std::map<Key, Bits> postings;
    std::string buf;
    std::size_t loaded = SIZE_MAX;
    for (const Key k : keys) {
        Bits& bits = postings.emplace(k, Bits(nblocks_)).first->second;
        auto it = std::upper_bound(chunks_.begin(), chunks_.end(), k,
                                   [](Key v, const Chunk& c) { return v < c.first_key; });
        if (it == chunks_.begin()) continue;
        const auto ci = static_cast<std::size_t>(it - chunks_.begin()) - 1;
        if (ci != loaded) {
            auto r = load_chunk(ci, read, buf);
            if (!r) return Unexpected(r.error());
            loaded = ci;
        }
        bool bad = false;
        const bool ok = walk_chunk(buf, chunks_[ci].first_key, [&](Key key, std::string_view bytes, bool bitmap) {
            if (key < k) return true;
            if (key == k && !decode_posting(bytes, bitmap, nblocks_, bits)) bad = true;
            return false;
        });
        if (!ok || bad) return make_error(Errc::corrupt, "index chunk malformed");
    }
    return evaluate(q, nblocks_, [&](Key k) -> const Bits& { return postings.at(k); });
}

Result<void> SealedIndex::for_each(const ReadFn& read,
                                   const std::function<void(Key, const std::vector<uint32_t>&)>& fn) const {
    std::string buf;
    std::vector<uint32_t> ids;
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        auto r = load_chunk(i, read, buf);
        if (!r) return r;
        bool bad = false;
        const bool ok = walk_chunk(buf, chunks_[i].first_key, [&](Key key, std::string_view bytes, bool bitmap) {
            Bits b(nblocks_);
            if (!decode_posting(bytes, bitmap, nblocks_, b)) {
                bad = true;
                return false;
            }
            ids.clear();
            for (std::size_t x = 0; x < nblocks_; ++x) {
                if (b.test(x)) ids.push_back(static_cast<uint32_t>(x));
            }
            fn(key, ids);
            return true;
        });
        if (!ok || bad) return make_error(Errc::corrupt, "index chunk malformed");
    }
    return {};
}

}  // namespace kt::store::trigram
