// The list of the Session History window: filters with counts per value, text
// search with the palette's matcher, grouping into sections, sorting. Pure data, no widgets:
// the window shows what build() returns, the tests check it directly.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "session/history_index.h"

namespace kt::app::history {

// Live: a pane shows it; Background: its host runs, no pane shows it; Closed.
enum class State : uint8_t { live, background, closed };
std::string_view to_string(State);

struct Item {
    session::HistoryEntry e;
    State state = State::closed;
    std::string commands;   // the command lines, newline separated (search); may be empty
};

enum class DateRange : uint8_t { any, today, yesterday, last_7_days, last_30_days, custom };
enum class Group : uint8_t { day, project, agent, tag, none };
enum class Sort : uint8_t { last_used, created, duration, size, commands };

// "*" in Filter::agents: any agent; "": no agent.
inline constexpr const char* kAnyAgent = "*";

struct Filter {
    std::string text;   // every word must be in a field (case-insensitive), or the whole text in the output
    std::set<State> states;                 // empty: all
    DateRange date = DateRange::any;
    uint64_t from_ms = 0, to_ms = 0;        // custom: last used in [from, to)
    std::set<std::string> projects, agents, tags;   // OR within a facet, AND across; "" = none
    bool agents_all = false;                        // agents: every value instead of any
    bool any() const {
        return !text.empty() || !states.empty() || date != DateRange::any || !projects.empty() || !agents.empty() || !tags.empty();
    }
};

// One value of a facet with the number of sessions it would show, given the other facets.
struct FacetValue {
    std::string key;    // what goes into the Filter ("" for no project / agent / tag)
    std::string label;
    std::size_t count = 0;
    bool on = false;
};

// Where the search text was found, for the row and the details: the field, its text and the
// byte ranges of the matches in it.
struct Snippet {
    enum class Field : uint8_t { title, project, folder, agent, tag, command, output };
    Field field = Field::title;
    std::string text;
    std::vector<std::pair<std::size_t, std::size_t>> marks;   // offset, length
    uint64_t ts_ms = 0;                                       // output: when it was printed
    uint64_t seq = 0;                                         // output: the line's stream offset
};
std::string_view to_string(Snippet::Field);

// A line of stored output with the search text (the window's background search).
struct OutputLine {
    std::string text;
    uint64_t ts_ms = 0;
    std::size_t offset = 0, length = 0;
    uint64_t seq = 0;   // the stream offset it was stored at (session/history_output.h)
};
using OutputMatches = std::map<std::string, std::vector<OutputLine>>;   // by history id

struct Section {
    std::string key;     // stable across rebuilds ("day:2026-11-25", "project:webapp"); folds by it
    std::string title;   // "" for Group::none
    std::vector<std::size_t> rows;   // indices into the items, in order
    bool folded = false;
};

struct View {
    std::vector<Section> sections;
    std::vector<FacetValue> states, dates, projects, agents, tags;
    std::size_t total = 0;   // all items
    std::size_t shown = 0;   // passing the filter (a row in several sections counts once)
    // Per item (when there is search text): what matched, fields first, then output lines.
    std::vector<std::vector<Snippet>> matches;
};

struct Options {
    Group group = Group::day;
    Sort sort = Sort::last_used;
    std::set<std::string> folded;   // section keys
    uint64_t now_ms = 0;
    // The local calendar day of a time (days since 1970-01-01 in local time); tests pass a
    // fixed one. Null: the system time zone.
    std::function<int64_t(uint64_t ms)> local_day;
    // The stored output lines with the whole search text, for Filter::text; null: none.
    const OutputMatches* output = nullptr;
    // The name of an agent id ("Claude Code"); null: the id.
    std::function<std::string(const std::string&)> agent_name;
};

View build(const std::vector<Item>& items, const Filter& f, const Options& o);

// "Today", "Yesterday", "Wednesday", "Last week", "3 weeks ago", "October 2026".
std::string day_title(int64_t day, int64_t today);
// The name a session goes by: its title, else its last folder, else its id.
std::string name_of(const session::HistoryEntry& e);
// Created to ended, or to now while it runs.
uint64_t duration_ms(const Item& it, uint64_t now_ms);
// "2h 40m", "45m", "30s".
std::string duration_text(uint64_t ms);
// The number of days since 1970-01-01 in the local time zone.
int64_t system_local_day(uint64_t ms);
// Minutes since the local midnight.
int system_local_minute(uint64_t ms);

// Local time as the window shows it; tests pass fixed functions.
struct Clock {
    std::function<int64_t(uint64_t ms)> day;    // null: system_local_day
    std::function<int(uint64_t ms)> minute;     // null: system_local_minute
    int64_t day_of(uint64_t ms) const { return day ? day(ms) : system_local_day(ms); }
    int minute_of(uint64_t ms) const { return minute ? minute(ms) : system_local_minute(ms); }
};
// "09:12".
std::string clock_text(const Clock&, uint64_t ms);
// "today 09:12", "yesterday 14:03", "Nov 18 10:30", "Nov 18 2025 10:30" (another year).
std::string when_text(const Clock&, uint64_t ms, uint64_t now_ms);
// "18h", "3d", "5w", "4mo": how long ago (closed badges).
std::string ago_text(uint64_t ms, uint64_t now_ms);

}  // namespace kt::app::history
