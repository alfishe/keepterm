#include "app/pane_sample.h"

#include <algorithm>

#include "agents/capture.h"
#include "agents/os_access.h"
#include "agents/registry.h"
#include "agents/session_files.h"
#include "session/client.h"

namespace kt::app {

namespace {

// agents::Source as the popover names it.
std::string source_name(uint8_t s) {
    switch (s) {
        case 1: return "agent data";
        case 2: return "model table";
        case 3: return "inferred";
        case 4: return "config";
        case 5: return "status line";
        case 6: return "estimate";
        default: return {};
    }
}

}  // namespace

std::string agent_of_chain(const std::vector<session::proto::ChainEntry>& chain) {
    struct Live {
        agents::StdFileSystem fs;
        agents::OsProcessProbe probe;
        agents::AgentRegistry reg = agents::AgentRegistry::builtin();
        Live() { agents::attach_builtin_sources(reg, fs, probe, agents::default_source_paths()); }
    };
    static const Live live;
    std::vector<agents::ProcessFacts> facts;
    for (const auto& e : chain) {
        agents::ProcessFacts f;
        f.argv = e.argv;
        f.image = e.image;
        f.comm = e.comm;
        f.pid = e.pid;
        f.start_time = e.start_unix_ns;
        f.cwd = e.cwd;
        facts.push_back(std::move(f));
    }
    auto m = agents::recognize_chain(facts, live.reg);
    return m && m->agent ? std::string(m->agent->id) : std::string();
}

std::string pane_cwd(const session::proto::ProcessInfo& pi, const std::string& agent) {
    if (!agent.empty() && pi.saved_agent == agent && !pi.saved_agent_dir.empty()) return pi.saved_agent_dir;
    if (!pi.cwd.empty()) return pi.cwd;
    for (auto e = pi.foreground_chain.begin(); e != pi.foreground_chain.end(); ++e)
        if (!e->cwd.empty()) return e->cwd;
    return {};
}

PaneSample sample_pane(const session::proto::ProcessInfo& pi, const std::string& endpoint, int context_timeout_ms) {
    PaneSample ps;
    ps.pid = pi.foreground_pid ? pi.foreground_pid : pi.root_pid;
    if (!pi.foreground_is_shell && !pi.foreground_chain.empty()) ps.process = pi.foreground_chain.back().image;
    ps.agent = agent_of_chain(pi.foreground_chain);
    if (!ps.agent.empty() && !endpoint.empty()) {
        using St = session::proto::AgentContextStatus;
        if (auto c = session::request_agent_context(endpoint, context_timeout_ms); c && (c->status == St::Ok || c->status == St::NoData))
            ps.context = std::move(*c);
    }
    ps.cwd = pane_cwd(pi, ps.agent);
    // The folder the agent registered for its session wins (Claude's pid file).
    if (ps.context && !ps.context->session_cwd.empty()) ps.cwd = ps.context->session_cwd;
    ps.saved_agent = pi.saved_agent;
    ps.saved_agent_session = pi.saved_agent_session;
    ps.agent_restore = pi.agent_restore;
    ps.at_prompt = pi.foreground_is_shell;
    if (!pi.foreground_is_shell) ps.command = command_of_chain(pi.foreground_chain);
    ps.shell = pi.shell_image;
    return ps;
}

std::string command_of_chain(const std::vector<session::proto::ChainEntry>& chain) {
    if (chain.empty()) return {};
    const auto& e = chain.front();
    std::string out;
    for (const std::string& a : e.argv) {
        if (a.empty()) continue;
        if (!out.empty()) out += ' ';
        out += a;
    }
    if (out.empty()) out = !e.comm.empty() ? e.comm : e.image;
    if (out.starts_with("-")) out.erase(0, 1);
    return out;
}

void apply_sample(const PaneSample& s, ui::PaneItem& out) {
    out.process = s.process;
    out.agent = s.agent;
    out.pid = s.pid;
    if (const auto& c = s.context) {
        out.agent_activity = c->activity;
        if (c->status == session::proto::AgentContextStatus::Ok && c->percent_x100)
            out.context = std::clamp(static_cast<float>(*c->percent_x100) / 10000.0f, 0.0f, 1.0f);
        out.context_tokens = c->used_tokens.value_or(0);
        out.context_limit = c->window_tokens.value_or(0);
        out.model = c->model;
        ui::AgentStats& a = out.agent_stats;
        a = {};
        a.valid = true;
        a.session_id = c->session_id;
        a.session_name = c->session_name;
        a.session_cwd = c->session_cwd;
        a.model = c->model;
        a.agent_version = c->agent_version;
        a.used_tokens = c->used_tokens.value_or(0);
        a.window_tokens = c->window_tokens.value_or(0);
        if (c->status == session::proto::AgentContextStatus::Ok && c->percent_x100) a.percent = static_cast<float>(*c->percent_x100) / 100.0f;
        a.input_tokens = c->input_tokens;
        a.output_tokens = c->output_tokens;
        a.cache_read_tokens = c->cache_read_tokens;
        a.cache_write_tokens = c->cache_write_tokens;
        if (c->cost_micro_usd) a.cost_usd = static_cast<double>(*c->cost_micro_usd) / 1e6;
        a.turns = c->turns;
        a.compactions = c->compactions;
        a.api_calls = c->api_calls;
        a.after_compaction = (c->flags & session::proto::kCtxAfterCompaction) != 0;
        a.used_from = source_name(c->used_source);
        a.window_from = source_name(c->window_source);
        a.updated_unix_ms = c->updated_unix_ms;
        a.limits_unix_ms = c->limits_unix_ms;
        for (const auto& l : c->limits)
            a.limits.push_back(ui::AgentLimitItem{l.name, static_cast<float>(l.used_percent_x100) / 100.0f, l.window_minutes, l.resets_at_unix});
    } else {
        out.agent_stats = {};
    }
    // The host's live answer: newer than the model's folder (which is also the respawn folder).
    if (!s.cwd.empty()) out.cwd = s.cwd;
    out.saved_agent = s.saved_agent;
    out.saved_agent_session = s.saved_agent_session;
    out.agent_restore = s.agent_restore;
    out.at_prompt = s.at_prompt;
}

}  // namespace kt::app
