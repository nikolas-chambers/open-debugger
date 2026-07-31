// Implements the Odbg_* flat C API declared in odbg_plugin_sdk.h and
// exported from odbg.exe, exactly the shape a real Olly plugin gets from
// linking against ollydbg.exe: a plugin DLL calls these by name after
// linking against odbg.lib. Every function is safe to call from any thread -
// it marshals onto the debug engine's worker thread itself (see
// DbgSession::RunOnWorker) if it isn't already on it.

#define ODBG_BUILDING_HOST
#include "odbg_plugin_sdk.h"
#include "dbgsession.h"
#include "settings.h"

#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Set by gui_main.cpp once the session exists.
extern DbgSession* g_session;

// Command names plugins register for the Command Reference window. Guarded by a
// mutex because Plugininit runs on the worker thread while the GUI thread reads
// the list each frame via HostGetPluginCommands().
static std::mutex g_pluginCmdMutex;
static std::vector<std::pair<std::string, std::string>> g_pluginCommands;

void Odbg_RegisterCommand(const char* name, const char* help) {
    if (!name) return;
    std::lock_guard<std::mutex> lk(g_pluginCmdMutex);
    for (auto& c : g_pluginCommands) if (c.first == name) { c.second = help ? help : ""; return; }
    g_pluginCommands.emplace_back(name, help ? help : "");
}

// Host-internal accessor (declared in gui_main.cpp). Returns a copy so the
// caller never holds the lock while rendering.
std::vector<std::pair<std::string, std::string>> HostGetPluginCommands() {
    std::lock_guard<std::mutex> lk(g_pluginCmdMutex);
    return g_pluginCommands;
}

bool Odbg_SessionActive() { return g_session && g_session->IsSessionActive(); }
bool Odbg_Stopped() { return g_session && g_session->IsStopped(); }

bool Odbg_Readmemory(unsigned long long addr, void* buf, unsigned long size) {
    return g_session && g_session->SafeReadMemory(addr, buf, size);
}

bool Odbg_Writememory(unsigned long long addr, const void* buf, unsigned long size) {
    return g_session && g_session->SafeWriteMemory(addr, buf, size);
}

unsigned long long Odbg_Getreg(const wchar_t* name) {
    return g_session ? g_session->SafeGetRegister(name) : 0;
}

bool Odbg_Setreg(const wchar_t* name, unsigned long long value) {
    return g_session && g_session->SafeSetRegister(name, value);
}

void Odbg_Getregs(OdbgRegs* out) {
    if (!out) return;
    *out = OdbgRegs{};
    if (!g_session) return;
    RegFile r = g_session->SafeGetRegisters();
    out->rax = r.rax; out->rbx = r.rbx; out->rcx = r.rcx; out->rdx = r.rdx;
    out->rsi = r.rsi; out->rdi = r.rdi; out->rbp = r.rbp; out->rsp = r.rsp;
    out->rip = r.rip; out->r8 = r.r8; out->r9 = r.r9; out->r10 = r.r10; out->r11 = r.r11;
}

int Odbg_Addbreakpoint(const wchar_t* moduleBangSymbolOrHexAddr) {
    return g_session ? g_session->SafeAddBreakpoint(moduleBangSymbolOrHexAddr) : -1;
}

bool Odbg_Removebreakpoint(int id) {
    return g_session && g_session->SafeRemoveBreakpoint(id);
}

unsigned long long Odbg_Getpeb() {
    return g_session ? g_session->SafeGetPeb() : 0;
}

void Odbg_Go() { if (g_session) g_session->SafeGo(); }
void Odbg_Pause() { if (g_session) g_session->SafePause(); }
void Odbg_Stepinto() { if (g_session) g_session->SafeStepInto(); }
void Odbg_Stepover() { if (g_session) g_session->SafeStepOver(); }

void Odbg_Log(const char* text) {
    if (g_session && text) g_session->LogFromPlugin(text);
}

// Per-plugin persisted settings. The key a plugin passes is namespaced with the
// name of whichever plugin is currently executing, so two plugins can both
// store "enabled" without stepping on each other. Called outside any plugin
// lifecycle function (which should not happen), the keys land under
// "plugin.(unknown)." rather than being silently dropped.
static std::string PluginSettingKey(const char* key) {
    const std::string& who = CurrentPluginName();
    return "plugin." + (who.empty() ? std::string("(unknown)") : who) + "." + key;
}

void Odbg_Setsetting(const char* key, const char* value) {
    if (!key || !*key) return;
    g_settings.Set(PluginSettingKey(key), value ? value : "");
}

int Odbg_Getsetting(const char* key, char* buf, int bufSize) {
    if (!key || !*key || !buf || bufSize <= 0) return -1;
    std::string v = g_settings.Get(PluginSettingKey(key));
    int n = (int)v.size();
    if (n > bufSize - 1) n = bufSize - 1;
    memcpy(buf, v.c_str(), (size_t)n);
    buf[n] = 0;
    return n;
}
