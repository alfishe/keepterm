// Settings writes: one key at a time into the user's config.toml, through
// the format-preserving editor (toml_edit.h), validated against the schema and written
// atomically.
//
// Every edit reads the file fresh, so an edit made in an editor since the last read is never
// lost: the change applies on top of what is on disk. A file that does not parse is never
// written; writable() reports why, and the Settings window turns read-only.
//
// Validation reuses the loader: the edited text is loaded with Config::load_text, and the edit
// is refused when the loader reports a problem with the key (wrong type, out of range, unknown
// enum value) or would not read back the value asked for. The live config is not touched here:
// the ConfigService's watcher (or reload_now) picks the new file up and publishes the change.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "base/error.h"
#include "config/config.h"

namespace kt::config {

// TOML text of a value as the Settings window writes it (canonical form, nothing redacted).
std::string value_text(const Value& v, ValueType type);

// A value typed into a Settings text field, converted for the key's type: numbers, durations
// ("10s"), sizes ("8MiB"), colors, comma-separated lists, `NAME=value` pairs for tables.
// The error is a short sentence for the field ("Enter a duration such as 500ms, 10s or 2m").
Result<Value> parse_input(const KeySpec& spec, std::string_view text);
// The inverse, for the field's text.
std::string input_text(const Value& v, ValueType type);

class ConfigWriter {
public:
    explicit ConfigWriter(std::filesystem::path file) : file_(std::move(file)) {}

    // The file Settings writes: `--config <file>` when given, else the profile's config.toml.
    static std::filesystem::path target_file(const LoadOptions& opts);

    const std::filesystem::path& file() const { return file_; }

    // Ok, or the reason the file cannot be edited (a syntax error with line and column).
    Result<void> writable() const;

    Result<void> set(KeyId k, const Value& v);
    // Removes the key's line: the default (or an include's value) applies again.
    Result<void> reset(KeyId k);
    // A [keys] entry: `toml_value` is the TOML text ("\"ctrl+d\"", "\"none\"",
    // "{ keys = \"ctrl+d\", global = true }").
    Result<void> set_binding(std::string_view command, std::string_view toml_value);
    Result<void> reset_binding(std::string_view command);

    // The text currently in the file (empty when it does not exist).
    Result<std::string> read() const;

    // Called after each successful write (the owner calls ConfigService::reload_now).
    std::function<void()> on_written;

private:
    Result<void> commit(const std::string& before, const std::string& after);
    std::filesystem::path file_;
};

// Writes `text` to `path` atomically: a temporary file next to it, flushed to disk, renamed
// over the target (MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, retried
// while an editor holds the file; rename(2) after fsync on POSIX). Creates the folder.
Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view text);

}  // namespace kt::config
