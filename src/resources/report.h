// Text and JSON output of a ResourceSnapshot. Core code.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "resources/model.h"
#include "resources/source.h"

namespace kt::resources {

struct ReportOptions {
    bool tree = false;     // session trees and the machine forest
    bool cpu = true;       // cpu_percent; null with --no-cpu
    std::string_view via = "direct";
    std::string_view main_metric = "private";      // JSON "metric" names
    std::string_view resident_metric = "resident";
    // On-demand details per node (--cmdline); empty function = not requested.
    std::function<ProcessDetails(const ProcessKey&)> details;
};

std::string to_json(const ResourceSnapshot&, const ReportOptions&);
std::string to_text(const ResourceSnapshot&, const ReportOptions&);

// "1.5 MiB", "312 KiB", "7 B".
std::string format_bytes(uint64_t);

}  // namespace kt::resources
