// fzf FuzzyMatchV2 scoring, ported from Macterm's SearchScoring / SearchText / SearchIndex
// (which port fzf's algo.go), with fzf's scoring constants and delimiter set.
#include "commands/matcher.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kt::commands::palette {

namespace {

// ---------------------------------------------------------------- UTF-8

// Decodes one scalar at s[i]; advances i. Invalid input yields U+FFFD for one byte.
char32_t next_scalar(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int len = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(len) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    i += static_cast<std::size_t>(len);
    return cp;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// ---------------------------------------------------------------- folding

bool is_combining_mark(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD) ||
           (c >= 0x064B && c <= 0x065F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x3099 || c == 0x309A;
}

// U+00C0..U+00FF and U+0100..U+017F to their base letters. Placeholders: 'A' = "ae",
// 'T' = "th", 'S' = "ss", 'I' = "ij", 'O' = "oe", '*' = the character itself.
constexpr std::string_view kLatin1 = "aaaaaaAceeeeiiiidnooooo*ouuuuyTSaaaaaaAceeeeiiiidnooooo*ouuuuyTy";
constexpr std::string_view kLatinA =
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiIIjjkkkllllllllllnnnnnnnnnooooooOOrrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

template <class Emit>
void emit_placeholder(char p, char32_t self, Emit&& emit) {
    switch (p) {
        case 'A': emit(U'a'); emit(U'e'); return;
        case 'T': emit(U't'); emit(U'h'); return;
        case 'S': emit(U's'); emit(U's'); return;
        case 'I': emit(U'i'); emit(U'j'); return;
        case 'O': emit(U'o'); emit(U'e'); return;
        case '*': emit(self); return;
        default: emit(static_cast<char32_t>(static_cast<unsigned char>(p))); return;
    }
}

char32_t fold_greek(char32_t c) {
    switch (c) {
        case 0x0386: case 0x03AC: return 0x03B1;   // ά → α
        case 0x0388: case 0x03AD: return 0x03B5;   // έ → ε
        case 0x0389: case 0x03AE: return 0x03B7;   // ή → η
        case 0x038A: case 0x03AF: case 0x0390: case 0x03AA: case 0x03CA: return 0x03B9;   // ί ΐ Ϊ ϊ → ι
        case 0x038C: case 0x03CC: return 0x03BF;   // ό → ο
        case 0x038E: case 0x03CD: case 0x03B0: case 0x03AB: case 0x03CB: return 0x03C5;   // ύ ΰ Ϋ ϋ → υ
        case 0x038F: case 0x03CE: return 0x03C9;   // ώ → ω
        case 0x03C2: return 0x03C3;                // ς → σ
        default: break;
    }
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 0x20;
    return c;
}

char32_t fold_cyrillic(char32_t c) {
    if (c >= 0x0400 && c <= 0x040F) c += 0x50;
    else if (c >= 0x0410 && c <= 0x042F) c += 0x20;
    switch (c) {
        case 0x0450: case 0x0451: return 0x0435;   // ѐ ё → е
        case 0x0453: return 0x0433;                // ѓ → г
        case 0x0457: return 0x0456;                // ї → і
        case 0x045C: return 0x043A;                // ќ → к
        case 0x045D: case 0x0439: return 0x0438;   // ѝ й → и
        case 0x045E: return 0x0443;                // ў → у
        default: return c;
    }
}

template <class Emit>
void fold_scalar(char32_t c, Emit&& emit) {
    if (c < 0x80) {
        emit(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
        return;
    }
    if (is_combining_mark(c)) return;
    if (c >= 0xC0 && c <= 0xFF) {
        emit_placeholder(kLatin1[c - 0xC0], c, emit);
        return;
    }
    if (c >= 0x100 && c <= 0x17F) {
        emit_placeholder(kLatinA[c - 0x100], c, emit);
        return;
    }
    if (c >= 0x0370 && c <= 0x03FF) {
        emit(fold_greek(c));
        return;
    }
    if (c >= 0x0400 && c <= 0x045F) {
        emit(fold_cyrillic(c));
        return;
    }
    if (c >= 0x0531 && c <= 0x0556) {   // Armenian capitals
        emit(c + 0x30);
        return;
    }
    if (c >= 0xFF21 && c <= 0xFF3A) {   // fullwidth A-Z
        emit(c - 0xFF21 + U'a');
        return;
    }
    if (c >= 0xFF41 && c <= 0xFF5A) {
        emit(c - 0xFF41 + U'a');
        return;
    }
    if (c >= 0xFF10 && c <= 0xFF19) {
        emit(c - 0xFF10 + U'0');
        return;
    }
    emit(c);
}

std::uint64_t mask_bit(std::uint32_t v) {
    if (v >= 'a' && v <= 'z') return 1ull << (v - 'a');
    if (v >= '0' && v <= '9') return 1ull << (26 + v - '0');
    return 1ull << (36 + v % 28);
}

// ---------------------------------------------------------------- character classes

enum class CharClass : std::uint8_t { White, NonWord, Delimiter, Lower, Upper, Letter, Number };

bool is_upper_cp(char32_t c) {
    if (c >= 0xC0 && c <= 0xDE) return c != 0xD7;
    if (c >= 0x100 && c <= 0x137) return (c % 2) == 0;
    if (c >= 0x139 && c <= 0x148) return (c % 2) == 1;
    if (c >= 0x14A && c <= 0x177) return (c % 2) == 0;
    if (c == 0x178) return true;
    if (c >= 0x179 && c <= 0x17E) return (c % 2) == 1;
    if ((c >= 0x0386 && c <= 0x038F) || (c >= 0x0391 && c <= 0x03AB)) return c != 0x0387 && c != 0x03A2;
    if (c >= 0x0400 && c <= 0x042F) return true;
    if (c >= 0x0531 && c <= 0x0556) return true;
    if (c >= 0xFF21 && c <= 0xFF3A) return true;
    return false;
}

bool is_lower_cp(char32_t c) {
    if (c >= 0xDF && c <= 0xFF) return c != 0xF7;
    if (c >= 0x100 && c <= 0x17F) return !is_upper_cp(c);
    if (c >= 0x03AC && c <= 0x03CE) return true;
    if (c >= 0x0430 && c <= 0x045F) return true;
    if (c >= 0x0561 && c <= 0x0587) return true;
    if (c >= 0xFF41 && c <= 0xFF5A) return true;
    return false;
}

CharClass class_of(char32_t c) {
    if (c < 0x80) {
        if (c >= 'a' && c <= 'z') return CharClass::Lower;
        if (c >= 'A' && c <= 'Z') return CharClass::Upper;
        if (c >= '0' && c <= '9') return CharClass::Number;
        if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return CharClass::White;
        switch (c) {
            case '/': case '\\': case ':': case ';': case ',': case '.': case '_': case '-': case '|':
                return CharClass::Delimiter;
            default: return CharClass::NonWord;
        }
    }
    if (c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000)
        return CharClass::White;
    if (is_upper_cp(c)) return CharClass::Upper;
    if (is_lower_cp(c)) return CharClass::Lower;
    if ((c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 || (c >= 0x2010 && c <= 0x205E) ||
        (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0x2190 && c <= 0x2BFF) || c == 0xFFFD)
        return CharClass::NonWord;
    if (c >= 0xFF10 && c <= 0xFF19) return CharClass::Number;
    if (c >= 0x0660 && c <= 0x0669) return CharClass::Number;
    return CharClass::Letter;
}

std::int16_t bonus_for(CharClass prev, CharClass cur) {
    if (cur != CharClass::White) {
        switch (prev) {
            case CharClass::White: return kBonusBoundaryWhite;
            case CharClass::Delimiter: return kBonusBoundaryDelimiter;
            case CharClass::NonWord: return kBonusBoundary;
            default: break;
        }
    }
    if ((prev == CharClass::Lower && cur == CharClass::Upper) ||
        (prev != CharClass::Number && cur == CharClass::Number))
        return kBonusCamel;
    switch (cur) {
        case CharClass::NonWord:
        case CharClass::Delimiter: return kBonusNonWord;
        case CharClass::White: return kBonusBoundaryWhite;
        default: return 0;
    }
}

bool is_query_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// ---------------------------------------------------------------- DP (Macterm's fill)

std::int32_t fill(const std::uint32_t* q, std::size_t m, const std::uint32_t* t, const std::int16_t* b,
                  std::size_t start, std::size_t last, Scratch& sc) {
    const std::int32_t* first = sc.first();
    const std::size_t width = last - start + 1;
    std::int32_t* matched = sc.buf(0);
    std::int32_t* gapped = sc.buf(1);
    std::int32_t* chunk = sc.buf(2);
    std::int32_t* prev_matched = sc.buf(3);
    std::int32_t* prev_gapped = sc.buf(4);
    std::int32_t* prev_chunk = sc.buf(5);

    // Row 0: the first term character; its bonus counts double.
    for (std::size_t c = 0; c < width; ++c) {
        const std::size_t j = start + c;
        if (t[j] == q[0]) {
            const std::int32_t bonus = b[j];
            matched[c] = kScoreMatch + bonus * kFirstCharMultiplier;
            chunk[c] = bonus;
        } else {
            matched[c] = kNoScore;
        }
        gapped[c] = c == 0 ? kNoScore : std::max(matched[c - 1] + kScoreGapStart, gapped[c - 1] + kScoreGapExtension);
    }
    for (std::size_t i = 1; i < m; ++i) {
        std::swap(matched, prev_matched);
        std::swap(gapped, prev_gapped);
        std::swap(chunk, prev_chunk);
        const std::uint32_t qi = q[i];
        const auto from = static_cast<std::size_t>(first[i]) - start;
        for (std::size_t c = 0; c < width; ++c) {
            if (c < from || c == 0) {
                matched[c] = kNoScore;
                gapped[c] = kNoScore;
                continue;
            }
            const std::size_t j = start + c;
            if (t[j] == qi) {
                const std::int32_t here = b[j];
                std::int32_t best = kNoScore;
                std::int32_t best_chunk = 0;
                // Consecutive with q[i-1] at c-1: the run's bonus carries, unless this
                // character starts a stronger word itself.
                const std::int32_t diagonal = prev_matched[c - 1];
                if (diagonal > kNoScore) {
                    const std::int32_t run = prev_chunk[c - 1];
                    if (here >= kBonusBoundary && here > run) {
                        best = diagonal + kScoreMatch + here;
                        best_chunk = here;
                    } else {
                        best = diagonal + kScoreMatch + std::max({here, run, kBonusConsecutive});
                        best_chunk = run;
                    }
                }
                // After a gap: this character's own bonus only.
                const std::int32_t after_gap = prev_gapped[c - 1];
                if (after_gap > kNoScore && after_gap + kScoreMatch + here > best) {
                    best = after_gap + kScoreMatch + here;
                    best_chunk = here;
                }
                matched[c] = best;
                chunk[c] = best_chunk;
            } else {
                matched[c] = kNoScore;
            }
            gapped[c] = std::max(matched[c - 1] + kScoreGapStart, gapped[c - 1] + kScoreGapExtension);
        }
    }
    std::int32_t best = kNoScore;
    for (std::size_t c = 0; c < width; ++c) best = std::max(best, matched[c]);
    return best;
}

// ---------------------------------------------------------------- thread pool

class Pool {
public:
    Pool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned n = hw > 1 ? hw - 1 : 0;
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { worker(); });
    }

    void run(std::size_t chunks, const std::function<void(std::size_t)>& fn) {
        std::lock_guard<std::mutex> serial(run_mutex_);
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &fn;
            total_ = chunks;
            next_.store(0);
            active_ = threads_.size();
            ++generation_;
        }
        cv_.notify_all();
        work();
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [&] { return active_ == 0; });
        job_ = nullptr;
    }

private:
    void work() {
        for (;;) {
            const std::size_t i = next_.fetch_add(1);
            if (i >= total_) break;
            (*job_)(i);
        }
    }
    void worker() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return generation_ != seen; });
                seen = generation_;
            }
            work();
            std::lock_guard<std::mutex> lk(m_);
            if (--active_ == 0) done_.notify_all();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex run_mutex_, m_;
    std::condition_variable cv_, done_;
    const std::function<void(std::size_t)>* job_ = nullptr;
    std::atomic<std::size_t> next_{0};
    std::size_t total_ = 0, active_ = 0;
    std::uint64_t generation_ = 0;
};

Pool& pool() {
    static Pool* p = new Pool();   // never destroyed: workers may still wait at process exit
    return *p;
}

std::size_t chunk_size(std::size_t total) {
    const std::size_t cores = std::max(1u, std::thread::hardware_concurrency());
    return std::max<std::size_t>(1024, total / (cores * 4));
}

}  // namespace

// ---------------------------------------------------------------- public

void parallel_for(std::size_t chunks, const std::function<void(std::size_t)>& fn) {
    if (chunks == 0) return;
    if (chunks == 1) {
        fn(0);
        return;
    }
    pool().run(chunks, fn);
}

SearchText prepare(std::string_view s) {
    SearchText out;
    out.scalars.reserve(s.size());
    out.bonus.reserve(s.size());
    out.offsets.reserve(s.size());
    CharClass previous = CharClass::White;
    std::size_t i = 0;
    std::uint32_t length = 0;
    while (i < s.size()) {
        const std::size_t at = i;
        const char32_t c = next_scalar(s, i);
        ++length;
        // A combining mark belongs to the character before it: it neither matches nor
        // starts a word.
        if (is_combining_mark(c)) continue;
        const CharClass cls = class_of(c);
        const std::int16_t here = bonus_for(previous, cls);
        bool first = true;
        fold_scalar(c, [&](char32_t f) {
            out.scalars.push_back(static_cast<std::uint32_t>(f));
            out.bonus.push_back(first ? here : std::int16_t{0});   // an expansion is one character
            out.offsets.push_back(static_cast<std::uint32_t>(at));
            out.mask |= mask_bit(static_cast<std::uint32_t>(f));
            first = false;
        });
        previous = cls;
    }
    out.length = length;
    return out;
}

SearchTerm make_term(std::string_view word) {
    SearchTerm t;
    std::size_t i = 0;
    while (i < word.size()) {
        const char32_t c = next_scalar(word, i);
        fold_scalar(c, [&](char32_t f) {
            t.scalars.push_back(static_cast<std::uint32_t>(f));
            t.mask |= mask_bit(static_cast<std::uint32_t>(f));
        });
    }
    return t;
}

std::string fold(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) fold_scalar(next_scalar(s, i), [&](char32_t f) { append_utf8(out, f); });
    return out;
}

SearchQuery SearchQuery::parse(std::string_view text) {
    SearchQuery q;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_query_space(text[i])) ++i;
        std::size_t j = i;
        while (j < text.size() && !is_query_space(text[j])) ++j;
        if (j > i) {
            SearchTerm t = make_term(text.substr(i, j - i));
            if (!t.scalars.empty()) q.terms.push_back(std::move(t));
        }
        i = j;
    }
    return q;
}

bool SearchQuery::narrows(const SearchQuery& previous) const {
    if (terms.size() < previous.terms.size()) return false;
    for (std::size_t i = 0; i < previous.terms.size(); ++i) {
        const auto& n = terms[i].scalars;
        const auto& o = previous.terms[i].scalars;
        if (n.size() < o.size() || !std::equal(o.begin(), o.end(), n.begin())) return false;
    }
    return true;
}

void Scratch::reserve(std::size_t rows, std::size_t columns) {
    if (first_.size() < rows) first_.resize(rows);
    if (bufs_[0].size() < columns) {
        const std::size_t cap = std::max(columns, bufs_[0].size() * 2);
        for (auto& b : bufs_) b.resize(cap);
    }
}

std::optional<std::int32_t> score(const SearchTerm& term, const SearchText& text, Scratch& sc) {
    const std::size_t m = term.scalars.size();
    if (m == 0) return 0;
    if ((text.mask & term.mask) != term.mask) return std::nullopt;
    const std::size_t n = text.scalars.size();
    if (n < m) return std::nullopt;
    const std::uint32_t* t = text.scalars.data();
    const std::uint32_t* q = term.scalars.data();
    sc.reserve(m, n);
    // Where each term character can first match, greedily: no alignment can place q[i]
    // earlier than first[i].
    std::int32_t* first = sc.first();
    std::size_t pi = 0;
    for (std::size_t j = 0; j < n && pi < m; ++j) {
        if (t[j] == q[pi]) first[pi++] = static_cast<std::int32_t>(j);
    }
    if (pi != m) return std::nullopt;
    // ...and the last character can match no later than its last occurrence.
    std::size_t last = n - 1;
    while (t[last] != q[m - 1]) --last;
    return fill(q, m, t, text.bonus.data(), static_cast<std::size_t>(first[0]), last, sc);
}

std::optional<std::vector<std::uint32_t>> positions(const SearchTerm& term, const SearchText& text) {
    const auto& q = term.scalars;
    const auto& t = text.scalars;
    const auto& b = text.bonus;
    const std::size_t m = q.size(), n = t.size();
    if (m == 0) return std::vector<std::uint32_t>{};
    if (n < m || (text.mask & term.mask) != term.mask) return std::nullopt;
    std::vector<std::int32_t> matched(m * n, kNoScore), gapped(m * n, kNoScore), chunk(m * n, 0);
    std::vector<char> via_gap(m * n, 0), gap_from_match(m * n, 0);
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t k = i * n + j;
            if (t[j] == q[i]) {
                const std::int32_t here = b[j];
                if (i == 0) {
                    matched[k] = kScoreMatch + here * kFirstCharMultiplier;
                    chunk[k] = here;
                } else if (j > 0) {
                    const std::size_t up = (i - 1) * n + j - 1;
                    std::int32_t best = kNoScore;
                    if (matched[up] > kNoScore) {
                        const std::int32_t run = chunk[up];
                        if (here >= kBonusBoundary && here > run) {
                            best = matched[up] + kScoreMatch + here;
                            chunk[k] = here;
                        } else {
                            best = matched[up] + kScoreMatch + std::max({here, run, kBonusConsecutive});
                            chunk[k] = run;
                        }
                    }
                    if (gapped[up] > kNoScore && gapped[up] + kScoreMatch + here > best) {
                        best = gapped[up] + kScoreMatch + here;
                        chunk[k] = here;
                        via_gap[k] = 1;
                    }
                    matched[k] = best;
                }
            }
            if (j > 0) {
                const std::int32_t from_match = matched[k - 1] + kScoreGapStart;
                const std::int32_t from_gap = gapped[k - 1] + kScoreGapExtension;
                gapped[k] = std::max(from_match, from_gap);
                gap_from_match[k] = from_match >= from_gap ? 1 : 0;
            }
        }
    }
    std::size_t j = n;
    std::int32_t best = kNoScore;
    for (std::size_t c = 0; c < n; ++c) {
        if (matched[(m - 1) * n + c] > best) {
            best = matched[(m - 1) * n + c];
            j = c;
        }
    }
    if (j == n) return std::nullopt;
    std::vector<std::uint32_t> out(m, 0);
    for (std::size_t i = m; i-- > 0;) {
        out[i] = static_cast<std::uint32_t>(j);
        if (i == 0) break;
        if (via_gap[i * n + j]) {
            // Walk the gap back to the match it opened from.
            std::size_t c = j - 1;
            while (!gap_from_match[(i - 1) * n + c]) --c;
            j = c - 1;
        } else {
            j -= 1;
        }
    }
    return out;
}

std::optional<std::int32_t> score_fields(const SearchQuery& q, std::span<const SearchText* const> fields, Scratch& sc) {
    std::int32_t total = 0;
    for (const auto& term : q.terms) {
        std::optional<std::int32_t> best;
        for (std::size_t pos = 0; pos < fields.size(); ++pos) {
            const auto s = score(term, *fields[pos], sc);
            if (!s) continue;
            const std::int32_t weighted = *s - static_cast<std::int32_t>(pos) * kFieldPenalty;
            if (!best || weighted > *best) best = weighted;
        }
        if (!best) return std::nullopt;
        total += *best;
    }
    return total;
}

std::optional<std::int32_t> score_fields(const SearchQuery& q, std::span<const SearchText> fields, Scratch& sc) {
    std::int32_t total = 0;
    for (const auto& term : q.terms) {
        std::optional<std::int32_t> best;
        for (std::size_t pos = 0; pos < fields.size(); ++pos) {
            const auto s = score(term, fields[pos], sc);
            if (!s) continue;
            const std::int32_t weighted = *s - static_cast<std::int32_t>(pos) * kFieldPenalty;
            if (!best || weighted > *best) best = weighted;
        }
        if (!best) return std::nullopt;
        total += *best;
    }
    return total;
}

std::vector<std::uint32_t> highlights(const SearchQuery& q, const SearchText& title) {
    std::vector<std::uint32_t> out;
    for (const auto& term : q.terms) {
        const auto p = positions(term, title);
        if (!p) continue;
        for (const auto i : *p) out.push_back(title.offsets[i]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool folded_has_prefix(std::string_view text, std::string_view prefix) {
    const SearchTerm a = make_term(text), b = make_term(prefix);
    return a.scalars.size() >= b.scalars.size() && std::equal(b.scalars.begin(), b.scalars.end(), a.scalars.begin());
}

// ---------------------------------------------------------------- SearchIndex

SearchIndex::SearchIndex(const std::vector<std::vector<std::string>>& records) {
    records_.resize(records.size());
    auto prep = [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            records_[i].reserve(records[i].size());
            for (const auto& f : records[i]) records_[i].push_back(prepare(f));
        }
    };
    if (records.size() < kParallelThreshold) {
        prep(0, records.size());
        return;
    }
    const std::size_t cs = chunk_size(records.size());
    const std::size_t chunks = (records.size() + cs - 1) / cs;
    parallel_for(chunks, [&](std::size_t c) { prep(c * cs, std::min(records.size(), (c + 1) * cs)); });
}

SearchIndex::SearchIndex(std::vector<std::vector<SearchText>> prepared) : records_(std::move(prepared)) {}

std::vector<Match> SearchIndex::match_serial(const SearchQuery& q) const {
    std::vector<Match> out;
    Scratch sc;
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (const auto s = score_fields(q, records_[i], sc)) out.push_back({static_cast<std::uint32_t>(i), *s});
    return out;
}

std::vector<Match> SearchIndex::match_all(const SearchQuery& q, const std::vector<std::uint32_t>* among) const {
    const std::size_t total = among ? among->size() : records_.size();
    std::vector<Match> out;
    auto index_at = [&](std::size_t p) { return among ? (*among)[p] : static_cast<std::uint32_t>(p); };
    if (q.empty()) {
        out.reserve(total);
        for (std::size_t p = 0; p < total; ++p) out.push_back({index_at(p), 0});
        return out;
    }
    auto range = [&](std::size_t lo, std::size_t hi, std::vector<Match>& dst, Scratch& sc) {
        for (std::size_t p = lo; p < hi; ++p) {
            const std::uint32_t i = index_at(p);
            if (const auto s = score_fields(q, records_[i], sc)) dst.push_back({i, *s});
        }
    };
    if (total < kParallelThreshold) {
        Scratch sc;
        range(0, total, out, sc);
        return out;
    }
    const std::size_t cs = chunk_size(total);
    const std::size_t chunks = (total + cs - 1) / cs;
    std::vector<std::vector<Match>> parts(chunks);
    parallel_for(chunks, [&](std::size_t c) {
        Scratch sc;
        range(c * cs, std::min(total, (c + 1) * cs), parts[c], sc);
    });
    std::size_t n = 0;
    for (const auto& p : parts) n += p.size();
    out.reserve(n);
    for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<Match> SearchIndex::rank(std::vector<Match> matches, std::size_t limit) const {
    auto better = [&](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        const std::uint32_t la = records_[a.index].empty() ? 0 : records_[a.index][0].length;
        const std::uint32_t lb = records_[b.index].empty() ? 0 : records_[b.index][0].length;
        if (la != lb) return la < lb;
        return a.index < b.index;
    };
    if (limit == 0 || limit >= matches.size()) {
        std::sort(matches.begin(), matches.end(), better);
        return matches;
    }
    // Bounded heap: the worst of the best `limit` on top.
    std::vector<Match> heap;
    heap.reserve(limit);
    for (const auto& m : matches) {
        if (heap.size() < limit) {
            heap.push_back(m);
            std::push_heap(heap.begin(), heap.end(), better);
        } else if (better(m, heap.front())) {
            std::pop_heap(heap.begin(), heap.end(), better);
            heap.back() = m;
            std::push_heap(heap.begin(), heap.end(), better);
        }
    }
    std::sort_heap(heap.begin(), heap.end(), better);
    return heap;
}

std::vector<Match> SearchIndex::search(const SearchQuery& q, std::size_t limit,
                                       const std::vector<std::uint32_t>* among) const {
    return rank(match_all(q, among), limit);
}

std::vector<Match> SearchSession::search(std::string_view text, std::size_t limit) {
    SearchQuery q = SearchQuery::parse(text);
    narrowed_ = last_ && q.narrows(*last_);
    std::vector<Match> all = index_->match_all(q, narrowed_ ? &last_matches_ : nullptr);
    last_matches_.clear();
    last_matches_.reserve(all.size());
    for (const auto& m : all) last_matches_.push_back(m.index);   // already in index order
    last_ = std::move(q);
    return index_->rank(std::move(all), limit);
}

}  // namespace kt::commands::palette
