// What the Settings window shows (Macterm's Settings panes): the panes in
// sidebar order, their groups, and one row per config key with its label, description and
// control. Defaults, ranges, enum values and docs come from the schema (schema.inc); this table
// only says where a key appears and how. Keys with no row are listed in Advanced → "Edit in
// File" (keys_without_row), so every key of the schema is visible somewhere.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "commands/keys.h"
#include "config/schema.h"
#include "ui/icons.h"

namespace kt::app::settings {

enum class Control : std::uint8_t {
    Auto,        // from the type: Bool toggle, small enum segmented, other enum dropdown,
                 // ranged float slider, small int range stepper, everything else a text field
    Toggle,
    Slider,
    Percent,     // a slider shown as 0-100 %
    Stepper,
    Dropdown,
    Segmented,
    Text,
    WideText,    // a text field under the label at full width (lists, tables)
    Color,
    FontFamily,  // a dropdown of the installed monospace families
    Preset,      // a dropdown of fixed values of a non-enum key (`options`: "24h=1 day|...");
                 // another value shows as "Custom: <value>"
};

// OS mask of a row (rows for keys that only one OS reads).
enum OsMask : std::uint8_t { kMac = 1, kWindows = 2, kLinux = 4, kAll = 7 };

struct RowDef {
    RowDef(std::string_view k, std::string_view l, std::string_view d, Control c = Control::Auto, double st = 0,
           std::string_view dep = {}, std::uint8_t o = kAll, std::string_view opts = {})
        : key(k), label(l), description(d), control(c), step(st), depends(dep), os(o), options(opts) {}
    std::string_view key;          // schema path
    std::string_view label;        // sentence case
    std::string_view description;  // "" = none (the schema doc is the tooltip)
    Control control = Control::Auto;
    double step = 0;               // sliders and steppers; 0 = from the range
    std::string_view depends;      // "key" (bool on) or "key=value" (enum); the row dims otherwise
    std::uint8_t os = kAll;
    std::string_view options;      // enum labels "a=Label|b=Label"; missing ones are humanized
};

struct GroupDef {
    GroupDef(std::string_view t, std::vector<RowDef> r, std::string_view f = {}) : title(t), rows(std::move(r)), footnote(f) {}
    std::string_view title;        // Title Case
    std::vector<RowDef> rows;
    std::string_view footnote;
};

// Panes with content beyond rows; the controller adds it.
enum class Special : std::uint8_t { None, General, Appearance, Keymaps, Agents, History, Storage, Advanced, QuickTerminal };

struct PaneDef {
    PaneDef(std::string_view i, std::string_view t, ui::Icon ic, Special sp, std::vector<GroupDef> g, std::uint8_t o = kAll)
        : id(i), title(t), icon(ic), special(sp), groups(std::move(g)), os(o) {}
    std::string_view id, title;
    ui::Icon icon;
    Special special = Special::None;
    std::vector<GroupDef> groups;
    std::uint8_t os = kAll;
};

const std::vector<PaneDef>& panes();
const PaneDef* find_pane(std::string_view id);

std::uint8_t os_bit(commands::Os os);
bool row_visible(const RowDef& r, commands::Os os);

// Schema keys that no row shows (Advanced → Edit in File), in schema order.
std::vector<config::KeyId> keys_without_row();
// Every row's key, for the coverage test (a row whose key is not in the schema is a bug).
std::vector<std::string_view> row_keys();

// "when-multiple" -> "When multiple"; the row's `options` labels win.
std::string option_label(const RowDef& r, std::string_view value);

}  // namespace kt::app::settings
