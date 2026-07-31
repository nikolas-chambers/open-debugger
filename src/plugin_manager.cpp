#include "plugin_manager.h"

#include <cstdio>

namespace {

std::string g_currentPlugin;

// Parks the calling plugin's name for the duration of one lifecycle call, so
// host exports invoked from inside it know who is asking.
struct PluginScope {
    explicit PluginScope(const std::string& name) : prev(g_currentPlugin) { g_currentPlugin = name; }
    ~PluginScope() { g_currentPlugin = prev; }
    std::string prev;
};

} // namespace

const std::string& CurrentPluginName() { return g_currentPlugin; }

PluginManager::~PluginManager() { CloseAll(); }

void PluginManager::LoadFromDirectory(const std::wstring& dir) {
    std::wstring pattern = dir + L"\\*.dll";
    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        std::wstring path = dir + L"\\" + fd.cFileName;
        HMODULE mod = LoadLibraryW(path.c_str());
        if (!mod) continue;

        auto plugindata = (Odbg_PlugindataFn)GetProcAddress(mod, "Odbg_Plugindata");
        auto plugininit = (Odbg_PlugininitFn)GetProcAddress(mod, "Odbg_Plugininit");
        if (!plugindata || !plugininit) { FreeLibrary(mod); continue; }

        char shortname[32] = {};
        plugindata(shortname);
        std::string name = shortname[0] ? shortname : "(unnamed plugin)";
        {
            // Named before init runs, so a plugin can already read its own
            // persisted settings from inside Odbg_Plugininit.
            PluginScope scope(name);
            if (!plugininit(ODBG_PLUGIN_ABI_VERSION)) { FreeLibrary(mod); continue; }
        }

        LoadedPlugin p;
        p.handle = mod;
        p.name = name;
        p.menuFn = (Odbg_PluginmenuFn)GetProcAddress(mod, "Odbg_Pluginmenu");
        p.actionFn = (Odbg_PluginactionFn)GetProcAddress(mod, "Odbg_Pluginaction");
        p.pausedFn = (Odbg_PausedFn)GetProcAddress(mod, "Odbg_Paused");
        p.closeFn = (Odbg_PlugincloseFn)GetProcAddress(mod, "Odbg_Pluginclose");

        if (p.menuFn) {
            PluginScope scope(p.name);
            char items[32][32] = {};
            int n = p.menuFn(ODBG_ORIGIN_PLUGINS_MENU, items, 32);
            for (int i = 0; i < n && i < 32; i++) p.menuItems.push_back(items[i]);
        }

        m_plugins.push_back(std::move(p));
    } while (FindNextFileW(find, &fd));

    FindClose(find);
}

void PluginManager::FireAction(size_t i, int action) {
    if (i >= m_plugins.size() || !m_plugins[i].actionFn) return;
    PluginScope scope(m_plugins[i].name);
    m_plugins[i].actionFn(ODBG_ORIGIN_PLUGINS_MENU, action);
}

void PluginManager::FirePaused(int reason, const OdbgRegs& regs) {
    for (auto& p : m_plugins) {
        if (!p.pausedFn) continue;
        PluginScope scope(p.name);
        p.pausedFn(reason, &regs);
    }
}

void PluginManager::CloseAll() {
    for (auto& p : m_plugins) {
        if (p.closeFn) {
            // Still named here, so a plugin's last act can be to persist its
            // settings from inside Odbg_Pluginclose.
            PluginScope scope(p.name);
            p.closeFn();
        }
        if (p.handle) FreeLibrary(p.handle);
    }
    m_plugins.clear();
}
