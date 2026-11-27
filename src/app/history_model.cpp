#include "app/history_model.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <map>

#include "base/utf8_range.h"

namespace kt::app::history {

namespace {

std::string project_of(const Item& it) { return it.e.project_name(); }

std::vector<std::string> agents_of(const Item& it) {
    std::vector<std::string> v;
    for (const auto& a : it.e.agents) {
        if (std::find(v.begin(), v.end(), a.agent) == v.end()) v.push_back(a.agent);
    }
    return v;
}

bool date_matches(DateRange d, uint64_t t, int64_t day, int64_t today, const Filter& f) {
    switch (d) {
        case DateRange::any: return true;
        case DateRange::today: return day == today;
        case DateRange::yesterday: return day == today - 1;
        case DateRange::last_7_days: return day > today - 7;
        case DateRange::last_30_days: return day > today - 30;
        case DateRange::custom: return t >= f.from_ms && (f.to_ms == 0 || t < f.to_ms);
    }
    return true;
}

constexpr std::size_t kMaxCommandSnippets = 5, kMaxOutputSnippets = 20;

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The lower-cased words of the search text.
std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> out;
    std::string w;
    for (char c : text + " ") {
        if (c == ' ' || c == '\t') {
            if (!w.empty()) out.push_back(std::move(w));
            w.clear();
        } else {
            w += fold(c);
        }
    }
    return out;
}

// Every offset of a lower-case word in a text, ASCII letters folded, not overlapping.
std::vector<std::size_t> find_all(const std::string& text, const std::string& word) {
    std::vector<std::size_t> at;
    if (word.empty() || word.size() > text.size()) return at;
    for (std::size_t i = 0; i + word.size() <= text.size();) {
        std::size_t k = 0;
        while (k < word.size() && fold(text[i + k]) == word[k]) ++k;
        if (k == word.size()) {
            at.push_back(i);
            i += word.size();
        } else {
            ++i;
        }
    }
    return at;
}

// Agents: any value (or every value with agents_all) holds; "*": some agent; "": none.
bool agents_match(const Filter& f, const std::vector<std::string>& have) {
    if (f.agents.empty()) return true;
    auto holds = [&](const std::string& want) {
        if (want == kAnyAgent) return !have.empty();
        if (want.empty()) return have.empty();
        return std::find(have.begin(), have.end(), want) != have.end();
    };
    return f.agents_all ? std::all_of(f.agents.begin(), f.agents.end(), holds)
                        : std::any_of(f.agents.begin(), f.agents.end(), holds);
}

template <class Set>
bool any_in(const Set& want, const std::vector<std::string>& have) {
    if (want.empty()) return true;
    if (have.empty()) return want.count("") > 0;
    return std::any_of(have.begin(), have.end(), [&](const std::string& h) { return want.count(h) > 0; });
}

// Which facets an item passes; a facet's counts use every other facet.
enum Facet : unsigned { kText = 1, kState = 2, kDate = 4, kProject = 8, kAgent = 16, kTag = 32, kAll = 63 };

struct Facts {
    unsigned pass = 0;   // the facets the item passes
    int64_t day = 0;
    int32_t score = 0;   // text match
};

std::string label_or(const std::string& s, const char* none) { return s.empty() ? std::string(none) : s; }

}  // namespace

std::string_view to_string(Snippet::Field f) {
    switch (f) {
        case Snippet::Field::title: return "title";
        case Snippet::Field::project: return "project";
        case Snippet::Field::folder: return "folder";
        case Snippet::Field::agent: return "agent";
        case Snippet::Field::tag: return "tag";
        case Snippet::Field::command: return "command";
        case Snippet::Field::output: return "output";
    }
    return "?";
}

std::string_view to_string(State s) {
    switch (s) {
        case State::live: return "live";
        case State::background: return "background";
        case State::closed: return "closed";
    }
    return "?";
}

int64_t system_local_day(uint64_t ms) {
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // Days from civil (Howard Hinnant's algorithm), for the local date.
    int y = tm.tm_year + 1900;
    const unsigned m = static_cast<unsigned>(tm.tm_mon + 1), d = static_cast<unsigned>(tm.tm_mday);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int system_local_minute(uint64_t ms) {
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm.tm_hour * 60 + tm.tm_min;
}

namespace {
struct Civil {
    int64_t y;
    unsigned m, d;
};
Civil civil(int64_t day) {
    const int64_t z = day + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<int64_t>(yoe) + era * 400 + (m <= 2), m, doy - (153 * mp + 2) / 5 + 1};
}
constexpr std::array<const char*, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                 "July",    "August",   "September", "October", "November", "December"};
}  // namespace

std::string clock_text(const Clock& c, uint64_t ms) {
    const int m = c.minute_of(ms);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", (m / 60) % 24, m % 60);
    return buf;
}

std::string when_text(const Clock& c, uint64_t ms, uint64_t now_ms) {
    const int64_t day = c.day_of(ms), today = c.day_of(now_ms);
    const std::string hm = clock_text(c, ms);
    if (day == today) return "today " + hm;
    if (day == today - 1) return "yesterday " + hm;
    const Civil a = civil(day), b = civil(today);
    std::string s = std::string(kMonths[a.m - 1]).substr(0, 3) + " " + std::to_string(a.d);
    if (a.y != b.y) s += " " + std::to_string(a.y);
    return s + " " + hm;
}

std::string ago_text(uint64_t ms, uint64_t now_ms) {
    const uint64_t s = now_ms > ms ? (now_ms - ms) / 1000 : 0;
    if (s < 60) return "now";
    if (s < 3600) return std::to_string(s / 60) + "m";
    if (s < 48 * 3600) return std::to_string(s / 3600) + "h";
    const uint64_t d = s / 86400;
    if (d < 14) return std::to_string(d) + "d";
    if (d < 60) return std::to_string(d / 7) + "w";
    if (d < 730) return std::to_string(d / 30) + "mo";
    return std::to_string(d / 365) + "y";
}

std::string name_of(const session::HistoryEntry& e) {
    std::string t = e.display_title();
    if (t.empty()) t = e.cwd_last.empty() ? e.id : e.cwd_last;
    return t;
}

std::string day_title(int64_t day, int64_t today) {
    static constexpr std::array<const char*, 7> kWeekdays = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    if (day >= today) return "Today";
    if (day == today - 1) return "Yesterday";
    // 1970-01-01 was a Thursday: weekday 3 with Monday = 0.
    auto weekday = [](int64_t d) { return static_cast<int>(((d % 7) + 7 + 3) % 7); };
    const int64_t week_start = today - weekday(today);
    if (day >= week_start) return kWeekdays[static_cast<std::size_t>(weekday(day))];
    const int64_t weeks = (week_start - day + 6) / 7;
    if (weeks == 1) return "Last week";
    if (weeks <= 4) return std::to_string(weeks) + " weeks ago";
    const Civil c = civil(day);
    return std::string(kMonths[c.m - 1]) + " " + std::to_string(c.y);
}

uint64_t duration_ms(const Item& it, uint64_t now_ms) {
    const uint64_t end = it.e.ended_ms ? *it.e.ended_ms : std::max(now_ms, it.e.last_used_ms);
    return end > it.e.created_ms ? end - it.e.created_ms : 0;
}

std::string duration_text(uint64_t ms) {
    const uint64_t s = ms / 1000;
    if (s < 60) return std::to_string(s) + "s";
    const uint64_t m = s / 60;
    if (m < 60) return std::to_string(m) + "m";
    const uint64_t h = m / 60;
    if (h < 48) return std::to_string(h) + "h" + (m % 60 ? " " + std::to_string(m % 60) + "m" : std::string());
    return std::to_string(h / 24) + "d" + (h % 24 ? " " + std::to_string(h % 24) + "h" : std::string());
}

View build(const std::vector<Item>& items, const Filter& f, const Options& o) {
    View v;
    v.total = items.size();
    const auto local_day = o.local_day ? o.local_day : system_local_day;
    const int64_t today = local_day(o.now_ms);

    // Text: every word must be in some field (title, project, folder, agents, tags, commands), or
    // the whole text in the session's stored output; what matched becomes the item's snippets.
    std::vector<int32_t> scores(items.size(), 0);
    std::vector<bool> text_ok(items.size(), true);
    const std::vector<std::string> words = words_of(f.text);
    std::string phrase;   // the whole text, as the output search looks for it
    for (const auto& w : words) phrase += (phrase.empty() ? "" : " ") + w;
    if (!words.empty()) {
        v.matches.resize(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& it = items[i];
            std::vector<Snippet> found;
            int32_t score = 0;
            std::vector<bool> word_found(words.size(), false);
            auto field = [&](Snippet::Field kind, const std::string& text, int32_t weight) {
                if (text.empty()) return;
                Snippet sn{kind, text, {}, 0};
                for (std::size_t w = 0; w < words.size(); ++w) {
                    const auto at = find_all(text, words[w]);
                    if (at.empty()) continue;
                    if (!word_found[w]) score += weight;
                    word_found[w] = true;
                    for (std::size_t pos : at) sn.marks.emplace_back(pos, words[w].size());
                }
                if (sn.marks.empty()) return;
                std::sort(sn.marks.begin(), sn.marks.end());
                found.push_back(std::move(sn));
            };
            field(Snippet::Field::title, name_of(it.e), 100);
            field(Snippet::Field::project, project_of(it), 60);
            for (const auto& t : it.e.tags) field(Snippet::Field::tag, t, 50);
            for (const auto& a : agents_of(it)) {
                const std::string name = o.agent_name ? o.agent_name(a) : a;
                field(Snippet::Field::agent, name == a ? a : name + " (" + a + ")", 50);
            }
            if (it.e.cwd_last != name_of(it.e)) field(Snippet::Field::folder, it.e.cwd_last, 40);
            if (it.e.cwd_first != it.e.cwd_last) field(Snippet::Field::folder, it.e.cwd_first, 40);
            std::size_t commands = 0;
            for (std::size_t from = 0; from < it.commands.size() && commands < kMaxCommandSnippets;) {
                std::size_t nl = it.commands.find('\n', from);
                if (nl == std::string::npos) nl = it.commands.size();
                const std::size_t before = found.size();
                field(Snippet::Field::command, it.commands.substr(from, nl - from), 30);
                commands += found.size() - before;
                from = nl + 1;
            }
            bool ok = std::all_of(word_found.begin(), word_found.end(), [](bool b) { return b; });
            if (!ok) found.clear();   // a field with only some of the words explains nothing
            if (o.output) {
                if (auto out = o.output->find(it.e.id); out != o.output->end() && !out->second.empty()) {
                    ok = true;
                    score += 10;
                    std::size_t n = 0;
                    for (const auto& line : out->second) {
                        if (n++ == kMaxOutputSnippets) break;
                        // Marked where the text is in the line (the store's offset as a fallback).
                        Snippet sn{Snippet::Field::output, line.text, {}, line.ts_ms, line.seq};
                        for (std::size_t at : find_all(line.text, phrase)) sn.marks.emplace_back(at, phrase.size());
                        if (sn.marks.empty())
                            if (auto r = utf8_whole_range(line.text, line.offset, line.length)) sn.marks.push_back(*r);
                        found.push_back(std::move(sn));
                    }
                }
            }
            text_ok[i] = ok;
            scores[i] = score;
            if (ok) v.matches[i] = std::move(found);
        }
    }

    std::vector<Facts> facts(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        Facts& x = facts[i];
        x.day = local_day(it.e.last_used_ms);
        x.score = scores[i];
        if (text_ok[i]) x.pass |= kText;
        if (f.states.empty() || f.states.count(it.state)) x.pass |= kState;
        if (date_matches(f.date, it.e.last_used_ms, x.day, today, f)) x.pass |= kDate;
        if (any_in(f.projects, {project_of(it)})) x.pass |= kProject;
        if (agents_match(f, agents_of(it))) x.pass |= kAgent;
        if (any_in(f.tags, it.e.tags)) x.pass |= kTag;
    }
    auto passes_but = [&](std::size_t i, unsigned facet) { return (facts[i].pass | facet) == kAll; };

    // Facet values with counts that follow the other facets.
    {
        std::array<std::size_t, 3> n{};
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (passes_but(i, kState)) ++n[static_cast<std::size_t>(items[i].state)];
        }
        for (State s : {State::live, State::background, State::closed}) {
            static constexpr std::array<const char*, 3> kLabels = {"Live", "Background", "Closed"};
            v.states.push_back({std::string(to_string(s)), kLabels[static_cast<std::size_t>(s)], n[static_cast<std::size_t>(s)],
                                f.states.count(s) > 0});
        }
    }
    {
        static constexpr std::array<std::pair<DateRange, const char*>, 4> kDates = {
            {{DateRange::today, "Today"}, {DateRange::yesterday, "Yesterday"}, {DateRange::last_7_days, "Last 7 days"},
             {DateRange::last_30_days, "Last 30 days"}}};
        for (const auto& [d, label] : kDates) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (passes_but(i, kDate) && date_matches(d, items[i].e.last_used_ms, facts[i].day, today, f)) ++n;
            }
            v.dates.push_back({std::to_string(static_cast<int>(d)), label, n, f.date == d});
        }
    }
    // Projects, agents, tags: every value any item has (so a value with 0 stays visible while
    // it is on), most frequent first, the "none" value last.
    auto facet = [&](unsigned bit, const std::set<std::string>& on, const char* none,
                     const std::function<std::vector<std::string>(const Item&)>& values_of) {
        std::map<std::string, std::size_t> n;
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto vals = values_of(items[i]);
            if (vals.empty()) vals.push_back("");
            for (const auto& val : vals) {
                auto& c = n[val];
                if (passes_but(i, bit)) ++c;
            }
        }
        for (const auto& k : on) n.try_emplace(k, 0);
        std::vector<FacetValue> out;
        for (const auto& [k, c] : n) out.push_back({k, label_or(k, none), c, on.count(k) > 0});
        std::stable_sort(out.begin(), out.end(), [](const FacetValue& a, const FacetValue& b) {
            if (a.key.empty() != b.key.empty()) return b.key.empty();
            return a.count > b.count;
        });
        return out;
    };
    v.projects = facet(kProject, f.projects, "No project", [](const Item& it) {
        auto p = project_of(it);
        return p.empty() ? std::vector<std::string>{} : std::vector<std::string>{p};
    });
    std::set<std::string> agents_on = f.agents;
    agents_on.erase(kAnyAgent);
    v.agents = facet(kAgent, agents_on, "No agent", agents_of);
    for (auto& a : v.agents)
        if (!a.key.empty() && o.agent_name) a.label = o.agent_name(a.key);
    {
        // "Any agent" first: the sessions with some agent that pass the other facets.
        std::size_t n = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (passes_but(i, kAgent) && !items[i].e.agents.empty()) ++n;
        if (n || f.agents.count(kAnyAgent))
            v.agents.insert(v.agents.begin(), FacetValue{kAnyAgent, "Any agent", n, f.agents.count(kAnyAgent) > 0});
    }
    v.tags = facet(kTag, f.tags, "Untagged", [](const Item& it) { return it.e.tags; });

    // The rows: sorted, then into sections.
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (facts[i].pass == kAll) rows.push_back(i);
    }
    v.shown = rows.size();
    auto key = [&](std::size_t i) -> uint64_t {
        const Item& it = items[i];
        switch (o.sort) {
            case Sort::last_used: return it.e.last_used_ms;
            case Sort::created: return it.e.created_ms;
            case Sort::duration: return duration_ms(it, o.now_ms);
            case Sort::size: return it.e.stats.stored_bytes;
            case Sort::commands: return it.e.stats.commands;
        }
        return 0;
    };
    const bool by_score = !f.text.empty();
    std::stable_sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) {
        if (by_score && facts[a].score != facts[b].score) return facts[a].score > facts[b].score;
        const uint64_t ka = key(a), kb = key(b);
        if (ka != kb) return ka > kb;
        return items[a].e.id < items[b].e.id;
    });

    // Sections in the order of their first row (the best one), except Day: newest day first.
    std::map<std::string, std::size_t> at;
    auto add = [&](const std::string& k, const std::string& title, std::size_t row) {
        auto [it, fresh] = at.try_emplace(k, v.sections.size());
        if (fresh) v.sections.push_back({k, title, {}, o.folded.count(k) > 0});
        v.sections[it->second].rows.push_back(row);
    };
    for (std::size_t r : rows) {
        const Item& it = items[r];
        switch (o.group) {
            case Group::day: {
                const std::string t = day_title(facts[r].day, today);
                add("day:" + t, t, r);
                break;
            }
            case Group::project: {
                const std::string p = project_of(it);
                add("project:" + p, label_or(p, "No project"), r);
                break;
            }
            case Group::agent: {
                const auto as = agents_of(it);
                if (as.empty()) add("agent:", "No agent", r);
                for (const auto& a : as) add("agent:" + a, a, r);
                break;
            }
            case Group::tag:
                if (it.e.tags.empty()) add("tag:", "Untagged", r);
                for (const auto& t : it.e.tags) add("tag:" + t, t, r);
                break;
            case Group::none: add("all", "", r); break;
        }
    }
    if (o.group == Group::day && (o.sort != Sort::last_used || by_score)) {
        // Rows sorted by another key (or by the search) still fall into days; the days stay
        // newest first.
        std::map<std::string, int64_t> newest;
        for (const auto& s : v.sections) {
            int64_t d = INT64_MIN;
            for (std::size_t r : s.rows) d = std::max(d, facts[r].day);
            newest[s.key] = d;
        }
        std::stable_sort(v.sections.begin(), v.sections.end(),
                         [&](const Section& a, const Section& b) { return newest[a.key] > newest[b.key]; });
    }
    return v;
}

}  // namespace kt::app::history
