#include "settings.h"

#include <cstdio>

SettingsStore g_settings;

void SettingsStore::Load(const std::string& path) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_kv.clear();
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        bool replaced = false;
        for (auto& kv : m_kv) if (kv.first == key) { kv.second = val; replaced = true; break; }
        if (!replaced) m_kv.emplace_back(key, val);
    }
    fclose(f);
}

bool SettingsStore::Save(const std::string& path) const {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "w");
    if (!f) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_kv) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
    return true;
}

std::string SettingsStore::Get(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& kv : m_kv) if (kv.first == key) return kv.second;
    return def;
}

int SettingsStore::GetInt(const std::string& key, int def) const {
    std::string v = Get(key);
    if (v.empty()) return def;
    return atoi(v.c_str());
}

void SettingsStore::Set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_kv) if (kv.first == key) { kv.second = value; return; }
    m_kv.emplace_back(key, value);
}

void SettingsStore::SetInt(const std::string& key, int value) {
    Set(key, std::to_string(value));
}
