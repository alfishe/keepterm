#include "agents/record_json.h"

namespace kt::agents {

std::string to_json(const AgentResume& record) {
    std::string out = "{\"agent\":";
    json::write_string(out, record.agent);
    out += ",\"session_id\":";
    json::write_string(out, record.session_id);
    out += ",\"directory\":";
    json::write_string(out, record.directory);
    out += ",\"arguments\":[";
    for (std::size_t i = 0; i < record.arguments.size(); ++i) {
        if (i != 0) out += ',';
        json::write_string(out, record.arguments[i]);
    }
    out += "]}";
    return out;
}

std::optional<AgentResume> agent_resume_from_json(const json::Value& object) {
    if (object.kind != json::Value::Kind::Object) return std::nullopt;
    auto agent = object.string_at("agent");
    auto session = object.string_at("session_id");
    auto directory = object.string_at("directory");
    if (!agent || !session || !directory || agent->empty() || session->empty() || directory->empty()) {
        return std::nullopt;
    }
    AgentResume r{std::move(*agent), std::move(*session), std::move(*directory), {}};
    if (const json::Value* args = object.get("arguments"); args != nullptr && args->kind != json::Value::Kind::Null) {
        if (args->kind != json::Value::Kind::Array) return std::nullopt;
        for (const json::Value& a : args->items) {
            if (a.kind != json::Value::Kind::String) return std::nullopt;
            r.arguments.push_back(a.text);
        }
    }
    return r;
}

std::optional<AgentResume> agent_resume_from_json(std::string_view text) {
    auto v = json::parse(text);
    if (!v) return std::nullopt;
    return agent_resume_from_json(*v);
}

std::optional<AgentResume> agent_resume_from_pane_json(std::string_view pane_object) {
    auto pane = json::parse(pane_object);
    if (!pane) return std::nullopt;
    const json::Value* record = pane->get(kPaneRecordKey);
    if (record == nullptr) return std::nullopt;
    return agent_resume_from_json(*record);
}

}  // namespace kt::agents
