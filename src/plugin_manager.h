#pragma once
// Loads plugin DLLs from a directory and drives their Olly-style lifecycle
// exports (Plugindata/Plugininit/Pluginmenu/Pluginaction/Paused/Pluginclose).
// Knows nothing about DbgHost/DbgSession - plugins reach the debug engine by
// calling the Odbg_* host exports declared in odbg_plugin_sdk.h directly,
// same as a real Olly plugin calls into ollydbg.exe.

#include "odbg_plugin_sdk.h"

#include <string>
#include <vector>
#include <windows.h>

struct LoadedPlugin {
    HMODULE handle = nullptr;
    std::string name;
    Odbg_PluginmenuFn menuFn = nullptr;
    Odbg_PluginactionFn actionFn = nullptr;
    Odbg_PausedFn pausedFn = nullptr;
    Odbg_PlugincloseFn closeFn = nullptr;
    std::vector<std::string> menuItems;
};

// Name of the plugin whose lifecycle function is running right now, or "" if we
// are not inside one. The flat Odbg_* C API has no way to tell which DLL called
// it, so PluginManager parks the name here around every plugin call and the host
// exports read it back - that is how per-plugin setting keys get namespaced.
const std::string& CurrentPluginName();

class PluginManager {
public:
    ~PluginManager();

    // Loads every *.dll in `dir` that exports Odbg_Plugindata + Odbg_Plugininit
    // and accepts our ABI version. Safe to call once at startup.
    void LoadFromDirectory(const std::wstring& dir);

    size_t Count() const { return m_plugins.size(); }
    const std::string& Name(size_t i) const { return m_plugins[i].name; }
    const std::vector<std::string>& MenuItems(size_t i) const { return m_plugins[i].menuItems; }

    void FireAction(size_t i, int action);
    // Must only be called from the debug engine's worker thread (regs come
    // straight from DbgHost, same thread-affinity rule as everything else).
    void FirePaused(int reason, const OdbgRegs& regs);

    void CloseAll();

private:
    std::vector<LoadedPlugin> m_plugins;
};
