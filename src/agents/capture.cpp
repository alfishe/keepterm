#include "agents/capture.h"

#include "agents/session_files.h"

namespace kt::agents {
namespace {

SessionQuery query_for(const ProcessFacts& facts, std::string_view pane_cwd) {
    return {facts.pid, facts.start_time, facts.cwd.empty() ? std::string(pane_cwd) : facts.cwd, facts.argv};
}

}  // namespace

std::optional<ChainMatch> recognize_chain(std::span<const ProcessFacts> chain, const AgentRegistry& registry,
                                          const CommandLineFetch& fetch) {
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const ProcessFacts& facts = chain[i];
        const Recognition r = recognize(facts, registry);
        if (r.match == Match::Yes) return ChainMatch{r.agent, i, facts.argv, false};
        if (r.match != Match::NeedsCommandLine) continue;

        // 1. A session file proves the agent by itself.
        const SessionQuery q = query_for(facts, {});
        for (const AgentDescriptor& d : registry.all()) {
            const ISessionSource* src = registry.source(d.id);
            if (src != nullptr && src->proves(q)) return ChainMatch{&d, i, {}, false};
        }
        // 2. The command line.
        if (!fetch) continue;
        auto argv = fetch(facts);
        if (!argv || argv->empty()) continue;
        ProcessFacts with_argv = facts;
        with_argv.argv = std::move(*argv);
        const Recognition again = recognize(with_argv, registry);
        if (again.match == Match::Yes) return ChainMatch{again.agent, i, std::move(with_argv.argv), true};
    }
    return std::nullopt;
}

std::optional<AgentResume> build_record(std::span<const ProcessFacts> chain, std::string_view pane_cwd,
                                        const AgentRegistry& registry, const std::optional<AgentResume>& current,
                                        const CommandLineFetch& fetch) {
    auto m = recognize_chain(chain, registry, fetch);
    if (!m) return std::nullopt;  // the shell or another program: the user ended the agent
    const AgentDescriptor& d = *m->agent;
    if (!d.resumable()) return std::nullopt;
    const ProcessFacts& facts = chain[m->index];
    const std::optional<AgentResume> keep =
        (current && current->agent == d.id) ? current : std::optional<AgentResume>{};

    std::vector<std::string> argv = std::move(m->argv);
    if (argv.empty() && !m->fetched && fetch) {  // proven by its session file: fetch for the flags
        if (auto fetched = fetch(facts)) argv = std::move(*fetched);
    }
    std::vector<std::string> args;
    if (argv.empty()) {
        if (keep) args = keep->arguments;  // command line unreadable: keep the known flags
    } else {
        if (!is_interactive(d, program_arguments(argv))) return std::nullopt;
        args = kept_arguments(d, argv);
    }

    const ISessionSource* src = registry.source(d.id);
    if (src == nullptr) return keep;
    SessionQuery q = query_for(facts, pane_cwd);
    if (q.argv.empty()) q.argv = argv;   // fetched on demand (Windows)
    auto info = src->session_of(q);
    if (!info) return keep;  // just started, no file yet
    if (d.requires_kind_interactive && info->kind && *info->kind != "interactive") return std::nullopt;

    std::string dir = info->cwd.value_or(std::string());
    if (dir.empty()) dir = facts.cwd;
    if (dir.empty()) dir = std::string(pane_cwd);
    if (dir.empty()) return keep;
    return AgentResume{std::string(d.id), std::move(info->session_id), std::move(dir), std::move(args)};
}

bool agent_is_ready(std::span<const ProcessFacts> chain, std::string_view agent_id, std::string_view pane_cwd,
                    const AgentRegistry& registry, const CommandLineFetch& fetch) {
    auto m = recognize_chain(chain, registry, fetch);
    if (!m || m->agent->id != agent_id) return false;
    const ISessionSource* src = registry.source(agent_id);
    return src != nullptr && src->is_ready(query_for(chain[m->index], pane_cwd));
}

}  // namespace kt::agents
