// The Session History window: filters on the left, the sessions grouped by
// day (or project, agent, tag) in the middle, the selected session's details and actions on the
// right. The controller builds the widgets from history::build() and runs the actions through
// the environment; the main window supplies the environment and the OS window.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "commands/target.h"
#include "app/history_model.h"
#include "base/error.h"
#include "session/history_export.h"
#include "ui/widgets.h"

namespace kt::ui {
class SearchField;
class Dropdown;
class Box;
class ScrollView;
}  // namespace kt::ui

namespace kt::app {

// What an export wrote (the toast names the file).
struct ExportResult {
    std::string path;
    bool unsized = false;   // asciicast of a series without stored sizes: played at 80x24
};
using ExportDone = std::function<void(Result<ExportResult>)>;

// Export…: asks for the format (text, raw, asciicast) in `w`; `chosen` runs with it, not at all
// on Cancel. The Session History window and the output viewer both use it.
void ask_export_format(ui::Window& w, std::function<void(session::ExportFormat)> chosen);
// The toast of an export's outcome in `w`.
void show_export_result(ui::Window& w, const Result<ExportResult>& r);

struct HistoryEnv {
    // Every series with its state; called when the window opens and on reload().
    std::function<std::vector<history::Item>()> load;
    // The commands of a series (the details column).
    std::function<std::vector<session::HistoryCommand>(const std::string& id)> commands;
    std::function<uint64_t()> now_ms;
    history::Clock clock;

    // The actions. Show: a live session's pane; Attach: a background one into a tab; Restore: a closed
    // one into a new tab (with `run`: and resume that agent run); Resume: a live or background
    // session's agent run.
    std::function<Result<void>(const history::Item&)> show, attach;
    // A restore starts its host in the background: an error returned is one found at once; `done`
    // (on the UI thread, may be empty) gets the outcome when the host started or failed and
    // returns whether it showed it; false: the caller shows it (in the main window).
    using RestoreDone = std::function<bool(const Result<void>&)>;
    std::function<Result<void>(const history::Item&, std::optional<std::size_t> run, RestoreDone done)> restore;
    std::function<Result<void>(const history::Item&, std::size_t run)> resume;
    std::function<Result<void>(const std::string& id, const std::string& title)> rename;
    std::function<Result<void>(const std::string& id, const std::vector<std::string>& add, const std::vector<std::string>& remove)> tag;
    std::function<Result<void>(const std::string& id, bool pinned)> pin;
    std::function<Result<uint64_t>(const std::string& id)> remove;
    std::function<void()> open_settings;   // the footer's gear: Settings → History
    // Searches the stored output of every session for the whole text (literal, any case) and
    // calls done on the window's thread; a newer call drops the older one. Null: no output search.
    std::function<void(const std::string& text, std::function<void(history::OutputMatches)> done)> search_output;
    // Jumps to a line of a session's output: a click on an output match (`text`: the search
    // text; `line`: the matching line), or on a command of a closed session (its command line).
    // A running session's pane finds `text`; a closed one opens its output viewer at the line.
    std::function<Result<void>(const history::Item&, const std::string& text, const std::optional<commands::HistoryLine>& line)>
        open_output;
    // Export…: the series' stored output in a format into a file the user picks (the save
    // chooser); `done` runs on the window's thread when it is written, not at all on cancel.
    std::function<void(const history::Item&, session::ExportFormat, ExportDone done)> export_output;
    // The retention line of the footer ("closed sessions are deleted after 1 month").
    std::function<std::string()> retention_note;
};

// What the window remembers between openings (the main window keeps it).
struct HistoryUiState {
    history::Filter filter;
    history::Group group = history::Group::day;
    history::Sort sort = history::Sort::last_used;
    std::set<std::string> folded;
    std::string selected;   // history id
};

class HistoryController {
public:
    explicit HistoryController(HistoryEnv env, HistoryUiState state = {});
    ~HistoryController();

    std::unique_ptr<ui::Widget> make_view();
    ui::Widget* view() const { return root_; }
    void closed();   // the window went away
    const HistoryUiState& state() const { return st_; }

    // Reads the sessions again (a session started, ended, was renamed elsewhere); the filters
    // and the selection stay.
    void reload();

    // Scripted use (tests, the "Show in Session History" command).
    void set_text(const std::string& text);
    void toggle_state(history::State);
    void set_date(history::DateRange);
    void toggle_project(const std::string&);
    void toggle_agent(const std::string&);
    void set_agents_all(bool);
    void toggle_tag(const std::string&);
    void clear_filters();
    void set_group(history::Group);
    void set_sort(history::Sort);
    void toggle_fold(const std::string& section_key);
    bool select(const std::string& id);   // false: not in the list (filtered out or unknown)
    const history::Item* selected() const;
    // Several sessions at once: Cmd/Ctrl+click adds or removes one, Shift+click and
    // Shift+arrows a range, Cmd/Ctrl+A all shown. Two or more: the details column acts on all
    // of them; Esc goes back to the one. `pick` is a click or a key on row `row` (public for tests).
    void pick(std::size_t row, uint16_t mods);
    void select_all();
    void clear_marked();
    const std::set<std::string>& marked() const { return marked_; }
    // The marked sessions in list order (none when fewer than two are marked).
    std::vector<const history::Item*> chosen() const;
    // The main action of the selected session: Show, Attach or Restore.
    void activate();
    // Asks to delete the selected session (closed ones only).
    void ask_delete();
    void ask_export();

    const history::View& model() const { return view_; }
    const std::vector<history::Item>& items() const { return items_; }
    // The rows of the list: a section header or a session.
    struct Row {
        bool header = false;
        std::size_t section = 0;
        std::size_t item = 0;
    };
    const std::vector<Row>& rows() const { return rows_; }

    std::function<void()> on_close;

private:
    void rebuild(bool keep_scroll = true);
    void build_filters();
    void build_chips();
    void build_details();
    void build_multi_details();
    // Runs `fn` on each chosen session (or the selected one), then reloads once; the first error is a toast.
    void run_each(const std::vector<const history::Item*>& items, const std::function<Result<void>(const history::Item&)>& fn);
    void build_footer();
    void paint_row(std::size_t i, ui::PaintContext&, ui::Rect, ui::ListView::RowState) const;
    void run(const Result<void>& r);
    // Runs fn on the next turn of the window: a click handler must not destroy its own widget.
    std::function<void()> later(std::function<void()> fn);
    void ask_rename();
    void ask_tag();
    void toast(std::string text, bool failure);
    // env_.restore with the outcome shown in this window while it is open (the main window is
    // behind it).
    void restore(const history::Item& it, std::optional<std::size_t> run);
    ui::Window* window() const;

    HistoryEnv env_;
    HistoryUiState st_;
    std::vector<history::Item> items_;
    history::View view_;
    std::set<std::string> marked_;            // history ids; empty or two and more
    std::optional<std::size_t> anchor_;       // the row a Shift range starts at
    std::vector<Row> rows_;
    std::vector<session::HistoryCommand> commands_;
    std::string commands_of_;   // the id commands_ belongs to
    history::OutputMatches output_;   // the output search's result for output_text_
    std::string output_text_;
    bool output_busy_ = false;        // a search for the current text runs
    bool commands_loaded_ = false;    // items_[].commands filled (on the first search)
    void search_output();

    ui::Widget* root_ = nullptr;
    ui::SearchField* search_ = nullptr;
    ui::Dropdown* group_ = nullptr;
    ui::Dropdown* sort_ = nullptr;
    ui::Box* chips_ = nullptr;
    ui::Box* filters_ = nullptr;
    ui::ListView* list_ = nullptr;
    ui::Box* details_ = nullptr;
    ui::ScrollView* details_scroll_ = nullptr;
    ui::Label* footer_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace kt::app
