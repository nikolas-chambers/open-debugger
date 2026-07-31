#pragma once
// One flat key=value store for everything odbg remembers between runs: the
// OS window rect, the theme, the OllyDbg-style debug options, per-pane scroll
// toggles, the last target that was opened - and whatever plugins choose to
// persist through Odbg_SetSetting/Odbg_GetSetting, which land in the same file
// under a "plugin." prefix.
//
// Dear ImGui's own imgui.ini still owns the dock/splitter layout; this is for
// state that lives outside imgui's model.
//
// Insertion order is preserved so the file stays diffable across runs, and the
// store is mutex-guarded because plugins write to it from the engine's worker
// thread while the GUI thread reads it.

#include <mutex>
#include <string>
#include <utility>
#include <vector>

class SettingsStore {
public:
    // Reads `path` if it exists; unknown keys are kept as-is and written back
    // out on Save, so a setting written by a newer build (or a plugin that
    // isn't loaded right now) is never silently dropped.
    void Load(const std::string& path);
    bool Save(const std::string& path) const;

    std::string Get(const std::string& key, const std::string& def = "") const;
    int         GetInt(const std::string& key, int def = 0) const;
    bool        GetBool(const std::string& key, bool def = false) const { return GetInt(key, def ? 1 : 0) != 0; }

    void Set(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int value);
    void SetBool(const std::string& key, bool value) { SetInt(key, value ? 1 : 0); }

private:
    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, std::string>> m_kv;
};

// The process-wide store, defined in settings.cpp. gui_main.cpp loads it at
// startup and saves it at exit; host_exports.cpp exposes it to plugins.
extern SettingsStore g_settings;
