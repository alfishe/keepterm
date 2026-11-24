// Format-preserving edits of a TOML document: the Settings window changes one
// value and leaves every comment, blank line and the order of keys and tables as the user wrote
// them. Core code: no OS calls.
//
// The editor scans the text into statements (table headers and key/value assignments, with the
// byte range of each value) without building a tree, so only the bytes of the changed value move:
//
//   set     an existing assignment gets the new value text (its end-of-line comment stays);
//           otherwise `key = value` goes after the last assignment of the key's table, or a
//           dotted key next to an assignment that already defines the table implicitly, or a
//           new `[table]` at the end of the file;
//   remove  deletes the assignment's line.
//
// Assignments inside arrays of tables ([[projects]]) are never matched: those entries are
// addressed by their owner modules, not by a key path. Every result is parsed again with toml++
// and must hold exactly the requested value at the path, otherwise the edit fails and the input
// stays as it was.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::config::toml_edit {

using Path = std::vector<std::string>;

// "font.size" -> {"font", "size"}. Schema paths only (no quoted segments).
Path split_path(std::string_view dotted);
// A key segment as TOML writes it: bare when possible, else a basic string.
std::string format_key(std::string_view key);
std::string format_path(const Path& p);

// Checks that `doc` parses. The error text has the line and column ("line 3, column 7: ...").
Result<void> check(std::string_view doc);

// The value text assigned to `path`, if the document assigns it directly.
std::optional<std::string> find(std::string_view doc, const Path& path);

// Sets `path` to `value` (TOML value text: `12.0`, `"Dracula"`, `["a", "b"]`).
Result<std::string> set(std::string_view doc, const Path& path, std::string_view value);

// Removes the assignment of `path`. `removed` (optional) tells whether there was one.
Result<std::string> remove(std::string_view doc, const Path& path, bool* removed = nullptr);

}  // namespace kt::config::toml_edit
