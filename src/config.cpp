#include "config.h"

#include "paths.h"
#include "strings.h"

#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <format>
#include <fstream>
#include <map>
#include <system_error>

namespace vcmic {
namespace {

struct Entry {
    std::wstring value;
    std::size_t line = 0;
    bool used = false;
};

// A deliberately small TOML subset: sections, `key = value`, `#`/`;` comments,
// quoted or bare scalars. No arrays, no nested tables, no dotted keys - the
// config never needs them, and this keeps the dependency list at zero.
class Document {
public:
    bool Parse(std::wstring_view text, std::vector<std::wstring>& errors) {
        std::wstring section;
        std::size_t line_number = 0;
        bool ok = true;

        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t end = text.find(L'\n', pos);
            std::wstring_view raw =
                text.substr(pos, end == std::wstring_view::npos ? text.size() - pos : end - pos);
            pos = (end == std::wstring_view::npos) ? text.size() + 1 : end + 1;
            ++line_number;

            const std::wstring_view line = Trim(raw);
            if (line.empty() || line.front() == L'#' || line.front() == L';') {
                continue;
            }

            if (line.front() == L'[') {
                const std::size_t close = line.find(L']');
                if (close == std::wstring_view::npos) {
                    errors.push_back(std::format(L"line {}: unterminated section header", line_number));
                    ok = false;
                    continue;
                }
                section = ToLowerInvariant(Trim(line.substr(1, close - 1)));
                continue;
            }

            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring_view::npos) {
                errors.push_back(std::format(L"line {}: expected 'key = value'", line_number));
                ok = false;
                continue;
            }

            const std::wstring key = ToLowerInvariant(Trim(line.substr(0, equals)));
            if (key.empty()) {
                errors.push_back(std::format(L"line {}: empty key", line_number));
                ok = false;
                continue;
            }

            std::wstring value;
            if (!ParseValue(Trim(line.substr(equals + 1)), line_number, value, errors)) {
                ok = false;
                continue;
            }

            const std::wstring full_key = section.empty() ? key : section + L"." + key;
            entries_[full_key] = Entry{value, line_number, false};
        }
        return ok;
    }

    Entry* Find(std::wstring_view key) {
        const auto it = entries_.find(std::wstring(key));
        return it == entries_.end() ? nullptr : &it->second;
    }

    void CollectUnused(std::vector<std::wstring>& warnings) const {
        for (const auto& [key, entry] : entries_) {
            if (!entry.used) {
                warnings.push_back(
                    std::format(L"line {}: unknown key '{}' (ignored)", entry.line, key));
            }
        }
    }

private:
    static bool ParseValue(std::wstring_view raw, std::size_t line_number, std::wstring& out,
                           std::vector<std::wstring>& errors) {
        out.clear();
        if (raw.empty()) {
            return true;
        }

        if (raw.front() == L'"') {
            std::size_t i = 1;
            bool closed = false;
            while (i < raw.size()) {
                const wchar_t c = raw[i];
                if (c == L'\\' && i + 1 < raw.size()) {
                    const wchar_t next = raw[i + 1];
                    switch (next) {
                        case L'n': out.push_back(L'\n'); break;
                        case L't': out.push_back(L'\t'); break;
                        case L'r': out.push_back(L'\r'); break;
                        case L'"': out.push_back(L'"'); break;
                        case L'\\': out.push_back(L'\\'); break;
                        default:
                            out.push_back(L'\\');
                            out.push_back(next);
                            break;
                    }
                    i += 2;
                    continue;
                }
                if (c == L'"') {
                    closed = true;
                    ++i;
                    break;
                }
                out.push_back(c);
                ++i;
            }
            if (!closed) {
                errors.push_back(std::format(L"line {}: unterminated string", line_number));
                return false;
            }
            const std::wstring_view rest = Trim(raw.substr(i));
            if (!rest.empty() && rest.front() != L'#' && rest.front() != L';') {
                errors.push_back(
                    std::format(L"line {}: unexpected text after the closing quote", line_number));
                return false;
            }
            return true;
        }

        // Bare value: a comment must be separated by whitespace so that values
        // containing '#' are still usable unquoted.
        std::size_t cut = raw.size();
        for (std::size_t i = 1; i < raw.size(); ++i) {
            if ((raw[i] == L'#' || raw[i] == L';') && (raw[i - 1] == L' ' || raw[i - 1] == L'\t')) {
                cut = i;
                break;
            }
        }
        out.assign(Trim(raw.substr(0, cut)));
        return true;
    }

    std::map<std::wstring, Entry> entries_;
};

class Reader {
public:
    Reader(Document& document, std::vector<std::wstring>& errors)
        : document_(document), errors_(errors) {}

    void String(const wchar_t* key, std::wstring& out) {
        if (Entry* entry = Take(key)) {
            out = entry->value;
        }
    }

    void Bool(const wchar_t* key, bool& out) {
        Entry* entry = Take(key);
        if (entry == nullptr) {
            return;
        }
        const std::wstring value = ToLowerInvariant(entry->value);
        if (value == L"true" || value == L"yes" || value == L"on" || value == L"1") {
            out = true;
        } else if (value == L"false" || value == L"no" || value == L"off" || value == L"0") {
            out = false;
        } else {
            errors_.push_back(std::format(L"line {}: '{}' expects true or false, got '{}'",
                                          entry->line, key, entry->value));
        }
    }

    void Double(const wchar_t* key, double& out, double min_value, double max_value) {
        Entry* entry = Take(key);
        if (entry == nullptr) {
            return;
        }
        wchar_t* end = nullptr;
        errno = 0;
        const double parsed = std::wcstod(entry->value.c_str(), &end);
        if (end == entry->value.c_str() || !Trim(std::wstring_view(end)).empty() || errno == ERANGE) {
            errors_.push_back(std::format(L"line {}: '{}' expects a number, got '{}'", entry->line,
                                          key, entry->value));
            return;
        }
        if (parsed < min_value || parsed > max_value) {
            errors_.push_back(std::format(L"line {}: '{}' must be between {} and {}, got {}",
                                          entry->line, key, min_value, max_value, parsed));
            return;
        }
        out = parsed;
    }

    template <class T>
    void Integer(const wchar_t* key, T& out, std::uint64_t min_value, std::uint64_t max_value) {
        Entry* entry = Take(key);
        if (entry == nullptr) {
            return;
        }
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::wcstoull(entry->value.c_str(), &end, 10);
        if (end == entry->value.c_str() || !Trim(std::wstring_view(end)).empty() || errno == ERANGE) {
            errors_.push_back(std::format(L"line {}: '{}' expects a whole number, got '{}'",
                                          entry->line, key, entry->value));
            return;
        }
        if (parsed < min_value || parsed > max_value) {
            errors_.push_back(std::format(L"line {}: '{}' must be between {} and {}, got {}",
                                          entry->line, key, min_value, max_value, parsed));
            return;
        }
        out = static_cast<T>(parsed);
    }

    void Level(const wchar_t* key, LogLevel& out) {
        Entry* entry = Take(key);
        if (entry == nullptr) {
            return;
        }
        if (!ParseLogLevel(entry->value, out)) {
            errors_.push_back(
                std::format(L"line {}: '{}' expects trace|debug|info|warn|error|off, got '{}'",
                            entry->line, key, entry->value));
        }
    }

private:
    Entry* Take(const wchar_t* key) {
        Entry* entry = document_.Find(key);
        if (entry != nullptr) {
            entry->used = true;
            if (entry->value.empty()) {
                return nullptr;  // an empty value means "leave the default alone"
            }
        }
        return entry;
    }

    Document& document_;
    std::vector<std::wstring>& errors_;
};

// TOML basic strings treat a backslash as an escape, so a Windows path written
// raw would come back with a tab in it the first time somebody points the log
// at C:\temp.
std::wstring Quote(std::wstring_view value) {
    std::wstring out;
    out.reserve(value.size() + 2);
    out.push_back(L'"');
    for (const wchar_t c : value) {
        if (c == L'"' || c == L'\\') {
            out.push_back(L'\\');
        }
        out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

// Shortest round-trip form, but never bare "0" for something the reader will
// hand to a double: a settings file where every number carries a decimal point
// is easier to edit without guessing which ones accept fractions.
std::wstring Decimal(double value) {
    std::wstring text = std::format(L"{}", value);
    if (text.find(L'.') == std::wstring::npos && text.find(L'e') == std::wstring::npos &&
        text.find(L"inf") == std::wstring::npos && text.find(L"nan") == std::wstring::npos) {
        text += L".0";
    }
    return text;
}

const wchar_t* BoolText(bool value) { return value ? L"true" : L"false"; }

// Every key the reader understands, with the comments that explain the ones
// worth changing. The tray rewrites this file, so it has to stand on its own:
// whatever the user had written here is gone after the first change made from
// the menu.
std::wstring Render(const Config& c) {
    std::wstring out;
    const auto line = [&out](std::wstring_view text) {
        out += text;
        out += L"\r\n";
    };
    const auto key = [&out](std::wstring_view name, std::wstring_view value,
                            std::wstring_view comment = {}) {
        out += std::format(L"{} = {}", name, value);
        if (!comment.empty()) {
            out += std::format(L"   # {}", comment);
        }
        out += L"\r\n";
    };

    line(L"# vcmic settings.");
    line(L"#");
    line(L"# Written by vcmic itself - the tray menu saves here after every change,");
    line(L"# rewriting the whole file. Hand edits survive until then; comments do not.");
    line(L"# 'Reload config' in the tray menu picks up changes made here.");
    line(L"");

    line(L"[devices]");
    line(L"# The endpoint id is what actually selects a device; run 'vcmic --list-devices'");
    line(L"# to see them all. The name is only a fallback for when an id stops existing -");
    line(L"# a driver reinstall or a different USB port - and matching by it logs a warning.");
    key(L"chat_render_id", Quote(c.devices.chat_render.id));
    key(L"chat_render_name", Quote(c.devices.chat_render.name_contains));
    key(L"mic_capture_id", Quote(c.devices.mic_capture.id));
    key(L"mic_capture_name", Quote(c.devices.mic_capture.name_contains));
    key(L"output_render_id", Quote(c.devices.output_render.id));
    key(L"output_render_name", Quote(c.devices.output_render.name_contains));
    line(L"");

    line(L"[audio]");
    key(L"sample_rate", std::format(L"{}", c.audio.sample_rate),
        L"every endpoint must already run at this rate");
    key(L"require_sample_rate", BoolText(c.audio.require_sample_rate),
        L"false only downgrades the error to a warning; vcmic never resamples");
    key(L"target_buffer_ms", Decimal(c.audio.target_buffer_ms),
        L"steady-state fill; lower is tighter, and closer to underrunning");
    key(L"ring_capacity_ms", Decimal(c.audio.ring_capacity_ms), L"headroom before an overrun");
    line(L"");

    line(L"[mix]");
    key(L"chat_gain_db", Decimal(c.mix.chat_gain_db));
    key(L"mic_gain_db", Decimal(c.mix.mic_gain_db));
    key(L"gain_smoothing_ms", Decimal(c.mix.gain_smoothing_ms),
        L"ramp applied to every gain change, including mute");
    key(L"limiter_enabled", BoolText(c.mix.limiter_enabled));
    key(L"limiter_threshold_db", Decimal(c.mix.limiter_threshold_db));
    key(L"limiter_release_ms", Decimal(c.mix.limiter_release_ms));
    line(L"");

    line(L"[gate]");
    line(L"# Noise gate on the microphone only. Off by default: Discord already gates");
    line(L"# what your friends hear, and this one decides what the recording gets.");
    key(L"enabled", BoolText(c.gate.enabled));
    key(L"threshold_db", Decimal(c.gate.threshold_db));
    key(L"attack_ms", Decimal(c.gate.attack_ms));
    key(L"hold_ms", Decimal(c.gate.hold_ms), L"keeps the gate open between words");
    key(L"release_ms", Decimal(c.gate.release_ms));
    line(L"");

    line(L"[drift]");
    line(L"# The cable and the sound card run off different crystals. This absorbs the");
    line(L"# difference by reading fractionally faster or slower, instead of dropping a");
    line(L"# block every few minutes.");
    key(L"enabled", BoolText(c.drift.enabled));
    key(L"measure_window_s", Decimal(c.drift.measure_window_s));
    key(L"response_s", Decimal(c.drift.response_s), L"keep this at least 3x measure_window_s");
    key(L"max_rate_correction", Decimal(c.drift.max_rate_correction), L"0.001 = 1000 ppm");
    line(L"");

    line(L"[resilience]");
    key(L"backoff_min_ms", std::format(L"{}", c.resilience.backoff_min_ms));
    key(L"backoff_max_ms", std::format(L"{}", c.resilience.backoff_max_ms));
    key(L"keep_chat_clock_alive", BoolText(c.resilience.keep_chat_clock_alive),
        L"silent render client, for cards whose loopback stops when nothing plays");
    key(L"startup_wait_s", std::format(L"{}", c.resilience.startup_wait_s),
        L"how long a fresh start waits for a device that is not there yet");
    line(L"");

    line(L"[log]");
    key(L"level", ToLowerInvariant(LogLevelName(c.log.level)), L"trace|debug|info|warn|error|off");
    key(L"file", Quote(c.log.file), L"relative paths resolve next to this config file");
    key(L"max_bytes", std::format(L"{}", c.log.max_bytes));
    key(L"keep_files", std::format(L"{}", c.log.keep_files));
    key(L"console", BoolText(c.log.console));
    key(L"stats_interval_s", std::format(L"{}", c.log.stats_interval_s), L"0 turns the report off");
    return out;
}

std::wstring ReadFileUtf8(const std::filesystem::path& path, bool& found) {
    found = false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::wstring();
    }
    found = true;
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return WideFromUtf8(bytes);
}

}  // namespace

ConfigLoad LoadConfigFile(const std::filesystem::path& path) {
    ConfigLoad result;

    const std::wstring text = ReadFileUtf8(path, result.file_exists);
    if (!result.file_exists) {
        result.ok = true;  // defaults are a valid configuration; the caller decides if that is enough
        return result;
    }

    Document document;
    const bool parsed = document.Parse(text, result.errors);

    Config& c = result.config;
    Reader reader(document, result.errors);

    reader.String(L"devices.chat_render_id", c.devices.chat_render.id);
    reader.String(L"devices.chat_render_name", c.devices.chat_render.name_contains);
    reader.String(L"devices.mic_capture_id", c.devices.mic_capture.id);
    reader.String(L"devices.mic_capture_name", c.devices.mic_capture.name_contains);
    reader.String(L"devices.output_render_id", c.devices.output_render.id);
    reader.String(L"devices.output_render_name", c.devices.output_render.name_contains);

    reader.Integer(L"audio.sample_rate", c.audio.sample_rate, 8000, 384000);
    reader.Bool(L"audio.require_sample_rate", c.audio.require_sample_rate);
    reader.Double(L"audio.target_buffer_ms", c.audio.target_buffer_ms, 2.0, 500.0);
    reader.Double(L"audio.ring_capacity_ms", c.audio.ring_capacity_ms, 20.0, 2000.0);

    reader.Double(L"mix.chat_gain_db", c.mix.chat_gain_db, -60.0, 24.0);
    reader.Double(L"mix.mic_gain_db", c.mix.mic_gain_db, -60.0, 24.0);
    reader.Double(L"mix.gain_smoothing_ms", c.mix.gain_smoothing_ms, 0.0, 500.0);
    reader.Bool(L"mix.limiter_enabled", c.mix.limiter_enabled);
    reader.Double(L"mix.limiter_threshold_db", c.mix.limiter_threshold_db, -24.0, 0.0);
    reader.Double(L"mix.limiter_release_ms", c.mix.limiter_release_ms, 1.0, 2000.0);

    reader.Bool(L"gate.enabled", c.gate.enabled);
    reader.Double(L"gate.threshold_db", c.gate.threshold_db, -90.0, 0.0);
    reader.Double(L"gate.attack_ms", c.gate.attack_ms, 0.1, 200.0);
    reader.Double(L"gate.hold_ms", c.gate.hold_ms, 0.0, 2000.0);
    reader.Double(L"gate.release_ms", c.gate.release_ms, 1.0, 2000.0);

    reader.Bool(L"drift.enabled", c.drift.enabled);
    reader.Double(L"drift.measure_window_s", c.drift.measure_window_s, 0.1, 30.0);
    reader.Double(L"drift.response_s", c.drift.response_s, 1.0, 120.0);
    reader.Double(L"drift.max_rate_correction", c.drift.max_rate_correction, 0.0, 0.02);

    reader.Integer(L"resilience.backoff_min_ms", c.resilience.backoff_min_ms, 10, 60000);
    reader.Integer(L"resilience.backoff_max_ms", c.resilience.backoff_max_ms, 10, 300000);
    reader.Bool(L"resilience.keep_chat_clock_alive", c.resilience.keep_chat_clock_alive);
    reader.Integer(L"resilience.startup_wait_s", c.resilience.startup_wait_s, 0, 3600);

    reader.Level(L"log.level", c.log.level);
    reader.String(L"log.file", c.log.file);
    reader.Integer(L"log.max_bytes", c.log.max_bytes, 0, 1024ull * 1024ull * 1024ull);
    reader.Integer(L"log.keep_files", c.log.keep_files, 0, 100);
    reader.Bool(L"log.console", c.log.console);
    reader.Integer(L"log.stats_interval_s", c.log.stats_interval_s, 0, 3600);

    document.CollectUnused(result.warnings);

    if (c.resilience.backoff_max_ms < c.resilience.backoff_min_ms) {
        result.errors.push_back(L"resilience.backoff_max_ms must be >= resilience.backoff_min_ms");
    }
    if (c.drift.enabled && c.drift.response_s < c.drift.measure_window_s * 3.0) {
        result.warnings.push_back(std::format(
            L"drift.response_s ({:.1f}) is less than 3x drift.measure_window_s ({:.1f}); the "
            L"correction loop reacts faster than it can measure and will hunt",
            c.drift.response_s, c.drift.measure_window_s));
    }
    if (c.audio.ring_capacity_ms < c.audio.target_buffer_ms * 3.0) {
        result.warnings.push_back(std::format(
            L"audio.ring_capacity_ms ({:.1f}) is less than 3x audio.target_buffer_ms ({:.1f}); "
            L"overruns become likely under load",
            c.audio.ring_capacity_ms, c.audio.target_buffer_ms));
    }

    result.ok = parsed && result.errors.empty();
    return result;
}

void ValidateForEngine(const Config& config, std::vector<std::wstring>& errors) {
    struct Required {
        const wchar_t* label;
        const wchar_t* id_key;
        const DeviceSelector* selector;
    };
    const Required required[] = {
        {L"chat loopback source", L"devices.chat_render_id", &config.devices.chat_render},
        {L"microphone", L"devices.mic_capture_id", &config.devices.mic_capture},
        {L"cable output", L"devices.output_render_id", &config.devices.output_render},
    };

    for (const Required& item : required) {
        if (item.selector->IsEmpty()) {
            errors.push_back(std::format(
                L"no device configured for the {}: set {} (run --list-devices to get the id)",
                item.label, item.id_key));
        }
    }
}

std::filesystem::path DefaultConfigPath() {
    const std::filesystem::path dir = AppDataDirectory();
    if (!dir.empty()) {
        return dir / L"config.toml";
    }
    const std::filesystem::path exe_dir = ExecutableDirectory();
    return exe_dir.empty() ? std::filesystem::path(L"config.toml") : exe_dir / L"config.toml";
}

ConfigLocation ResolveConfigPath(const std::filesystem::path& override_path) {
    ConfigLocation location;
    std::error_code ignored;

    if (!override_path.empty()) {
        location.path = std::filesystem::absolute(override_path, ignored);
        if (ignored) {
            location.path = override_path;
        }
        location.from_command_line = true;
        location.exists = std::filesystem::exists(location.path, ignored);
        return location;
    }

    const std::filesystem::path app_data = DefaultConfigPath();
    if (std::filesystem::exists(app_data, ignored)) {
        location.path = app_data;
        location.exists = true;
        return location;
    }

    const std::filesystem::path exe_dir = ExecutableDirectory();
    if (!exe_dir.empty()) {
        const std::filesystem::path beside = exe_dir / L"config.toml";
        if (std::filesystem::exists(beside, ignored)) {
            location.path = beside;
            location.exists = true;
            location.portable = true;
            return location;
        }
    }

    location.path = app_data;  // nothing yet; this is where a first run puts one
    return location;
}

std::filesystem::path ResolveRelativeTo(const std::filesystem::path& directory,
                                        const std::wstring& path) {
    if (path.empty()) {
        return std::filesystem::path();
    }
    std::filesystem::path candidate(path);
    if (candidate.is_absolute()) {
        return candidate;
    }
    if (!directory.empty()) {
        return directory / candidate;
    }
    const std::filesystem::path exe_dir = ExecutableDirectory();
    return exe_dir.empty() ? candidate : exe_dir / candidate;
}

bool SaveConfigFile(const std::filesystem::path& path, const Config& config, std::wstring& error) {
    error.clear();

    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        if (code && !std::filesystem::is_directory(directory, code)) {
            error = std::format(L"cannot create {}", directory.wstring());
            return false;
        }
    }

    const std::string text = Utf8FromWide(Render(config));

    // Written beside the target and then moved over it, so a config that is
    // being rewritten while Windows shuts down is either the old one or the new
    // one, never half of each.
    std::filesystem::path temporary = path;
    temporary += L".new";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            error = std::format(L"cannot write {}", temporary.wstring());
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream.good()) {
            error = std::format(L"writing {} failed", temporary.wstring());
            return false;
        }
    }

    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        error = std::format(L"cannot replace {}", path.wstring());
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

}  // namespace vcmic
