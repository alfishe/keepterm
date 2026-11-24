// The AgentResume record in the pane object of the workspace state file:
//   "agent": { "agent": "claude", "session_id": "...", "directory": "...", "arguments": [...] }
// Core code.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "agents/agent_resume.h"
#include "agents/json_lite.h"

namespace kt::agents {

// The key of the record in the pane object.
inline constexpr std::string_view kPaneRecordKey = "agent";

// The record as a compact JSON object.
std::string to_json(const AgentResume& record);

// From a JSON object. nullopt when it is not an object or "agent", "session_id" or "directory" is
// missing, not a string or empty. "arguments" is optional (missing = none); a non-string element
// rejects the record. Unknown keys are ignored. An unknown agent id is kept.
std::optional<AgentResume> agent_resume_from_json(const json::Value& object);
std::optional<AgentResume> agent_resume_from_json(std::string_view text);

// The record under kPaneRecordKey of a pane object. A missing key, null, or an invalid record
// decodes as none, so pane objects written before the record existed still load.
std::optional<AgentResume> agent_resume_from_pane_json(std::string_view pane_object);

}  // namespace kt::agents
