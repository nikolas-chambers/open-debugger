// odbg - OllyDbg-style front end for the debugger core.
//
// Win32 + DirectX11 host for Dear ImGui, laid out as the classic Olly
// four-pane CPU window: disassembly (main), registers (top-right),
// stack (bottom-right), dump (bottom-left), a command bar you can type
// Olly-style verbs into, and a log. A DbgSession runs the actual DbgEng
// session on its own thread and also serves the same command vocabulary
// over a named pipe (\\.\pipe\odbg_cmd), so an external script can drive
// (and watch) the same session as the person at the keyboard.

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "dbgsession.h"
#include "settings.h"

#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <unordered_set>

// Self-crash handler: we're building the debugger, so when odbg itself
// faults there's no other debugger around to catch it. Print the exception
// code/address as a module-relative offset (cross-reference the /MAP file)
// plus a raw return-address stack so the bug is findable without one.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
    ULONG64 base = (ULONG64)mod;
    ULONG64 addr = (ULONG64)ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "\n[CRASH] code=0x%08x addr=0x%llx module_base=0x%llx offset=0x%llx\n",
        ep->ExceptionRecord->ExceptionCode, (unsigned long long)addr,
        (unsigned long long)base, (unsigned long long)(addr - base));
    fflush(stderr);

    void* frames[32] = {};
    USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    fprintf(stderr, "[CRASH] stack (%u frames, offsets from module base 0x%llx):\n", n, (unsigned long long)base);
    for (USHORT i = 0; i < n; i++) {
        fprintf(stderr, "  [%u] 0x%llx (off 0x%llx)\n", i,
            (unsigned long long)frames[i], (unsigned long long)((ULONG64)frames[i] - base));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static bool                    g_SwapChainOccluded = false;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Color themes. Swap the active preset from View > Theme.
// ---------------------------------------------------------------------------

struct Theme {
    const char* name;
    ImVec4 windowBg, paneBg, addr, bytes, mnemonic, reg, imm, symbol, text, dim;
    ImVec4 currentLineBg, changedReg, breakpointRow, selectedRow;
};

static const Theme kThemes[] = {
    // Classic Olly: black background, amber/green/cyan accents.
    { "Classic Olly",
      ImVec4(0.00f, 0.00f, 0.00f, 1.00f), ImVec4(0.00f, 0.00f, 0.00f, 1.00f),
      ImVec4(0.55f, 0.55f, 0.55f, 1.00f), ImVec4(0.45f, 0.45f, 0.45f, 1.00f),
      ImVec4(0.95f, 0.85f, 0.30f, 1.00f), ImVec4(0.35f, 0.80f, 0.95f, 1.00f),
      ImVec4(0.90f, 0.40f, 0.90f, 1.00f), ImVec4(0.40f, 0.85f, 0.40f, 1.00f),
      ImVec4(0.85f, 0.85f, 0.85f, 1.00f), ImVec4(0.45f, 0.45f, 0.45f, 1.00f),
      ImVec4(0.15f, 0.25f, 0.35f, 1.00f), ImVec4(0.95f, 0.25f, 0.25f, 1.00f),
      ImVec4(0.35f, 0.10f, 0.10f, 1.00f), ImVec4(0.20f, 0.20f, 0.20f, 1.00f) },
    // Dark Modern: VS-Code-like blues/purples.
    { "Dark Modern",
      ImVec4(0.09f, 0.09f, 0.10f, 1.00f), ImVec4(0.09f, 0.09f, 0.10f, 1.00f),
      ImVec4(0.50f, 0.55f, 0.60f, 1.00f), ImVec4(0.40f, 0.40f, 0.45f, 1.00f),
      ImVec4(0.55f, 0.75f, 1.00f, 1.00f), ImVec4(0.65f, 0.85f, 0.55f, 1.00f),
      ImVec4(0.85f, 0.65f, 0.95f, 1.00f), ImVec4(0.90f, 0.75f, 0.45f, 1.00f),
      ImVec4(0.80f, 0.80f, 0.82f, 1.00f), ImVec4(0.45f, 0.45f, 0.50f, 1.00f),
      ImVec4(0.16f, 0.22f, 0.33f, 1.00f), ImVec4(1.00f, 0.45f, 0.45f, 1.00f),
      ImVec4(0.30f, 0.14f, 0.14f, 1.00f), ImVec4(0.22f, 0.23f, 0.26f, 1.00f) },
    // Solarized Dark.
    { "Solarized Dark",
      ImVec4(0.00f, 0.17f, 0.21f, 1.00f), ImVec4(0.00f, 0.17f, 0.21f, 1.00f),
      ImVec4(0.40f, 0.48f, 0.51f, 1.00f), ImVec4(0.35f, 0.43f, 0.46f, 1.00f),
      ImVec4(0.71f, 0.54f, 0.00f, 1.00f), ImVec4(0.15f, 0.55f, 0.82f, 1.00f),
      ImVec4(0.83f, 0.21f, 0.51f, 1.00f), ImVec4(0.52f, 0.60f, 0.00f, 1.00f),
      ImVec4(0.51f, 0.58f, 0.59f, 1.00f), ImVec4(0.35f, 0.43f, 0.46f, 1.00f),
      ImVec4(0.03f, 0.21f, 0.26f, 1.00f), ImVec4(0.86f, 0.20f, 0.18f, 1.00f),
      ImVec4(0.25f, 0.10f, 0.10f, 1.00f), ImVec4(0.07f, 0.26f, 0.31f, 1.00f) },
};
static int g_themeIndex = 0;
static const Theme& CurTheme() { return kThemes[g_themeIndex]; }

// ---------------------------------------------------------------------------
// Persisted window placement + options. Dear ImGui's own imgui.ini already
// remembers pane/dock layout once we stop force-rebuilding it every launch
// (see the DockBuilderGetNode check in main()); everything outside imgui's
// model - the OS window rect, the theme, the debug options, the per-pane
// scroll toggles, the last target opened, and any plugin's own settings -
// lives in the shared SettingsStore, written to a sibling ini next to the exe.
// ---------------------------------------------------------------------------

// Absolute path to a file living next to the exe (so settings and the imgui
// layout ini are found no matter what working directory we were launched from -
// which matters here, since we deliberately launch from a target's install dir).
static std::string ExeSiblingPath(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring w(path);
    size_t slash = w.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : w.substr(0, slash);
    std::wstring full = dir + L"\\" + name;
    int n = WideCharToMultiByte(CP_ACP, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_ACP, 0, full.c_str(), -1, &a[0], n, nullptr, nullptr);
    return a;
}

static std::string SettingsPath() { return ExeSiblingPath(L"odbg_settings.ini"); }

static RECT g_lastWinRect{};
static bool g_haveWinRect = false;

// ---------------------------------------------------------------------------
// Disassembly syntax highlighting: a light heuristic tokenizer, not a real
// x86 grammar - good enough to color mnemonics/registers/immediates/symbols
// distinctly, which is the point of "nice code highlighting" here.
// ---------------------------------------------------------------------------

static bool IsRegisterToken(const std::string& t) {
    static const std::unordered_set<std::string> regs = {
        "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","rip",
        "eax","ebx","ecx","edx","esi","edi","ebp","esp","eip",
        "ax","bx","cx","dx","si","di","bp","sp",
        "al","bl","cl","dl","ah","bh","ch","dh",
        "r8","r9","r10","r11","r12","r13","r14","r15",
        "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d",
        "cs","ds","es","fs","gs","ss",
    };
    return regs.count(t) != 0;
}

static bool IsHexNumber(const std::string& t) {
    if (t.empty()) return false;
    size_t i = 0;
    if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) i = 2;
    if (i >= t.size()) return false;
    for (; i < t.size(); i++) {
        char c = t[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'h';
        if (!hex) return false;
    }
    return true;
}

// Renders `text` (already split off from the mnemonic) token by token, using
// delimiters as visual separators (dim) and classifying alnum runs as
// register/immediate/symbol/default.
static void DrawColoredOperands(const std::string& text, const Theme& th) {
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ',' || c == '[' || c == ']' || c == '+' || c == '-' || c == '*' || c == ' ' || c == ':') {
            ImGui::PushStyleColor(ImGuiCol_Text, th.dim);
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(&text[i], &text[i] + 1);
            ImGui::PopStyleColor();
            i++;
            continue;
        }
        size_t start = i;
        while (i < text.size() && text[i] != ',' && text[i] != '[' && text[i] != ']' &&
               text[i] != '+' && text[i] != '-' && text[i] != '*' && text[i] != ' ' && text[i] != ':')
            i++;
        std::string tok = text.substr(start, i - start);
        ImVec4 col = th.text;
        if (IsRegisterToken(tok)) col = th.reg;
        else if (IsHexNumber(tok) || (!tok.empty() && isdigit((unsigned char)tok[0]))) col = th.imm;
        else if (!tok.empty()) col = th.symbol;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(tok.c_str());
        ImGui::PopStyleColor();
    }
}

// Addresses are shown 0x-prefixed, at a width that suits the target: a 32-bit
// debuggee reads as 0x00401000 rather than being buried in eight leading zeros,
// a 64-bit one keeps all sixteen digits. The width is decided once per view (by
// the widest address in it) so a column never jitters between rows.
static int AddrDigitsFor(ULONG64 maxAddr) { return maxAddr > 0xFFFFFFFFull ? 16 : 8; }

static void FormatAddr(char* buf, size_t bufSize, ULONG64 addr, int digits) {
    sprintf_s(buf, bufSize, "0x%0*llX", digits, (unsigned long long)addr);
}

static void DrawDisasmText(const DisasmLine& dl, const Theme& th, int digits) {
    char addr[32];
    FormatAddr(addr, sizeof(addr), dl.addr, digits);

    ImGui::PushStyleColor(ImGuiCol_Text, th.addr);
    ImGui::TextUnformatted(addr);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    char bytesbuf[64];
    // dl.bytes is the raw wide hex string produced by DbgHost; render at a
    // fixed width so the disasm column lines up.
    std::string bytesA;
    for (wchar_t wc : dl.bytes) bytesA += (char)wc;
    sprintf_s(bytesbuf, "%-24s", bytesA.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, th.bytes);
    ImGui::TextUnformatted(bytesbuf);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    std::string textA;
    for (wchar_t wc : dl.text) textA += (char)wc;
    size_t sp = textA.find(' ');
    std::string mnem = sp == std::string::npos ? textA : textA.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : textA.substr(sp + 1);

    ImGui::PushStyleColor(ImGuiCol_Text, th.mnemonic);
    ImGui::TextUnformatted(mnem.c_str());
    ImGui::PopStyleColor();
    if (!rest.empty()) {
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(" ");
        DrawColoredOperands(rest, th);
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Toolbar icon buttons: small square glyph buttons drawn with ImDrawList
// primitives (no icon atlas available), styled after Olly's classic button
// strip - flat colored silhouettes, tight spacing, hover tooltips.
// ---------------------------------------------------------------------------

typedef void (*IconDrawFn)(ImDrawList*, ImVec2, ImVec2, ImU32);

static bool ToolbarIconButton(const char* id, const char* tooltip, ImVec4 color, IconDrawFn drawIcon) {
    ImGui::PushID(id);
    float sz = 26.0f * ImGui::GetFontSize() / 15.0f;
    bool clicked = ImGui::Button("", ImVec2(sz, sz));
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    drawIcon(ImGui::GetWindowDrawList(), mn, mx, ImGui::ColorConvertFloat4ToU32(color));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

static void ToolbarSeparator() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 3, p.y + 2), ImVec2(p.x + 3, p.y + h - 2),
        ImGui::GetColorU32(ImGuiCol_Separator), 1.5f);
    ImGui::Dummy(ImVec2(8, h));
    ImGui::SameLine();
}

static void IconOpen(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    dl->AddRectFilled(ImVec2(mn.x + w * 0.18f, mn.y + h * 0.24f), ImVec2(mn.x + w * 0.55f, mn.y + h * 0.38f), col);
    dl->AddRect(ImVec2(mn.x + w * 0.18f, mn.y + h * 0.38f), ImVec2(mx.x - w * 0.18f, mx.y - h * 0.22f), col, 1.0f, 0, 1.6f);
}

static void IconAttach(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float cx = (mn.x + mx.x) * 0.5f;
    dl->AddRect(ImVec2(mn.x + w * 0.24f, mn.y + h * 0.60f), ImVec2(mx.x - w * 0.24f, mx.y - h * 0.16f), col, 0, 0, 1.6f);
    dl->AddLine(ImVec2(cx, mn.y + h * 0.14f), ImVec2(cx, mn.y + h * 0.52f), col, 2.0f);
    dl->AddTriangleFilled(ImVec2(cx - 5, mn.y + h * 0.40f), ImVec2(cx + 5, mn.y + h * 0.40f), ImVec2(cx, mn.y + h * 0.58f), col);
}

static void IconRun(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    dl->AddTriangleFilled(ImVec2(mn.x + w * 0.32f, mn.y + h * 0.20f), ImVec2(mn.x + w * 0.32f, mx.y - h * 0.20f),
        ImVec2(mx.x - w * 0.24f, (mn.y + mx.y) * 0.5f), col);
}

static void IconPause(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float barW = w * 0.16f;
    dl->AddRectFilled(ImVec2(mn.x + w * 0.28f, mn.y + h * 0.20f), ImVec2(mn.x + w * 0.28f + barW, mx.y - h * 0.20f), col);
    dl->AddRectFilled(ImVec2(mx.x - w * 0.28f - barW, mn.y + h * 0.20f), ImVec2(mx.x - w * 0.28f, mx.y - h * 0.20f), col);
}

static void IconStepInto(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float cx = (mn.x + mx.x) * 0.5f;
    dl->AddLine(ImVec2(cx, mn.y + h * 0.16f), ImVec2(cx, mn.y + h * 0.56f), col, 2.0f);
    dl->AddTriangleFilled(ImVec2(cx - 5, mn.y + h * 0.46f), ImVec2(cx + 5, mn.y + h * 0.46f), ImVec2(cx, mn.y + h * 0.64f), col);
    dl->AddLine(ImVec2(mn.x + w * 0.20f, mx.y - h * 0.22f), ImVec2(mx.x - w * 0.20f, mx.y - h * 0.22f), col, 2.0f);
}

static void IconStepOver(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    dl->AddRectFilled(ImVec2(mn.x + w * 0.40f, mx.y - h * 0.34f), ImVec2(mx.x - w * 0.40f, mx.y - h * 0.20f), col);
    ImVec2 p0(mn.x + w * 0.20f, mx.y - h * 0.24f);
    ImVec2 p1(mn.x + w * 0.20f, mn.y + h * 0.12f);
    ImVec2 p2(mx.x - w * 0.20f, mn.y + h * 0.12f);
    ImVec2 p3(mx.x - w * 0.20f, mx.y - h * 0.30f);
    dl->AddBezierCubic(p0, p1, p2, p3, col, 2.0f);
    dl->AddTriangleFilled(ImVec2(p3.x - 5, p3.y - 8), ImVec2(p3.x + 5, p3.y - 8), ImVec2(p3.x, p3.y), col);
}

static void IconRet(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float cx = (mn.x + mx.x) * 0.5f;
    dl->AddLine(ImVec2(cx, mx.y - h * 0.16f), ImVec2(cx, mn.y + h * 0.34f), col, 2.0f);
    dl->AddTriangleFilled(ImVec2(cx - 5, mn.y + h * 0.44f), ImVec2(cx + 5, mn.y + h * 0.44f), ImVec2(cx, mn.y + h * 0.26f), col);
    dl->AddLine(ImVec2(mn.x + w * 0.20f, mx.y - h * 0.16f), ImVec2(mx.x - w * 0.20f, mx.y - h * 0.16f), col, 2.0f);
}

// Rewind: restart the current target from the top. Drawn as Olly's classic
// "back to start" bar-plus-triangle rather than a circular arrow, so it reads
// as "run it again from the beginning" next to the transport buttons.
static void IconRestart(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float cy = (mn.y + mx.y) * 0.5f;
    dl->AddRectFilled(ImVec2(mn.x + w * 0.20f, mn.y + h * 0.24f),
                      ImVec2(mn.x + w * 0.28f, mx.y - h * 0.24f), col);
    dl->AddTriangleFilled(ImVec2(mx.x - w * 0.20f, mn.y + h * 0.22f),
                          ImVec2(mx.x - w * 0.20f, mx.y - h * 0.22f),
                          ImVec2(mn.x + w * 0.32f, cy), col);
}

// Kill: terminate the debuggee and end the session.
static void IconKill(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    ImVec2 a(mn.x + w * 0.28f, mn.y + h * 0.28f), b(mx.x - w * 0.28f, mx.y - h * 0.28f);
    dl->AddLine(a, b, col, 2.2f);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), col, 2.2f);
}

// Horizontal double-arrow, for the per-pane scroll toggle.
static void IconScroll(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col) {
    float w = mx.x - mn.x, h = mx.y - mn.y;
    float cy = (mn.y + mx.y) * 0.5f;
    float x0 = mn.x + w * 0.22f, x1 = mx.x - w * 0.22f;
    float t = h * 0.16f;
    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), col, 1.6f);
    dl->AddTriangleFilled(ImVec2(x0, cy), ImVec2(x0 + t, cy - t), ImVec2(x0 + t, cy + t), col);
    dl->AddTriangleFilled(ImVec2(x1, cy), ImVec2(x1 - t, cy - t), ImVec2(x1 - t, cy + t), col);
}

// The small button that sits at the right of a pane's header and turns that
// pane's own horizontal scrollbar on or off. This used to be a single global
// "Horizontal scroll in Dump" checkbox buried in the Options menu; per pane and
// one click away is both more useful and closer to what the panes actually need
// (the disassembly overflows far more often than the dump does).
static bool PaneScrollToggle(const char* id, bool* on, const Theme& th) {
    float sz = ImGui::GetFrameHeight() * 0.80f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > sz) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - sz);
    ImGui::PushID(id);
    bool clicked = ImGui::Button("", ImVec2(sz, sz));
    if (clicked) *on = !*on;
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImVec4 col = *on ? ImVec4(0.35f, 0.85f, 0.35f, 1.0f) : th.dim;
    IconScroll(ImGui::GetWindowDrawList(), mn, mx, ImGui::ColorConvertFloat4ToU32(col));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Horizontal scroll in this pane: %s", *on ? "on" : "off");
    ImGui::PopID();
    return clicked;
}

static void BuildDockLayout(ImGuiID dockspaceId, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    // Every split below consumes a node and produces two fresh ones; each
    // node id is used exactly once as an input to avoid the aliasing bug
    // that previously nested Stack inside the wrong parent.
    ImGuiID leftCol = 0, rightCol = 0;
    rightCol = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right, 0.30f, nullptr, &leftCol);

    // Left column: CPU on top, Dump below.
    ImGuiID cpuNode = 0, dumpNode = 0;
    dumpNode = ImGui::DockBuilderSplitNode(leftCol, ImGuiDir_Down, 0.35f, nullptr, &cpuNode);

    // Right column: Registers, Stack, Command, Log stacked top to bottom.
    ImGuiID regNode = 0, rightRest1 = 0;
    rightRest1 = ImGui::DockBuilderSplitNode(rightCol, ImGuiDir_Down, 0.60f, nullptr, &regNode);
    ImGuiID stackNode = 0, rightRest2 = 0;
    rightRest2 = ImGui::DockBuilderSplitNode(rightRest1, ImGuiDir_Down, 0.60f, nullptr, &stackNode);
    ImGuiID cmdNode = 0, logNode = 0;
    logNode = ImGui::DockBuilderSplitNode(rightRest2, ImGuiDir_Down, 0.80f, nullptr, &cmdNode);

    ImGui::DockBuilderDockWindow("CPU", cpuNode);
    ImGui::DockBuilderDockWindow("Dump", dumpNode);
    ImGui::DockBuilderDockWindow("Registers", regNode);
    ImGui::DockBuilderDockWindow("Stack", stackNode);
    ImGui::DockBuilderDockWindow("Command", cmdNode);
    ImGui::DockBuilderDockWindow("Log", logNode);
    ImGui::DockBuilderFinish(dockspaceId);
}

DbgSession* g_session = nullptr;  // extern-linked: host_exports.cpp reaches the session through this.
static bool g_showOpenPopup = false;
static bool g_showAttachPopup = false;
static char g_launchBuf[512] = "";
static char g_attachFilter[128] = "";
static DWORD g_attachSelectedPid = 0;

// Per-pane horizontal scroll, toggled by the small button in each pane header
// (persisted, like every other option).
static bool g_scrollCpu = false;
static bool g_scrollDump = false;
static bool g_scrollStack = false;
static bool g_scrollRegs = false;
static bool g_scrollLog = false;
static bool g_scrollTerm = true;

// Last target opened, and whether to put it back in the Open dialog (and offer
// to relaunch it) next run.
static bool g_rememberLastExe = true;
static char g_lastExe[512] = "";

// CPU window: which process tab is showing, and the row the user has clicked.
static ULONG   g_cpuTabProc = 0;       // engine id of the selected tab
static ULONG   g_seenProcSerial = 0;   // last Snapshot::procSerial we snapped to
static ULONG   g_procRequested = 0;    // last `proc` asked for but not yet reflected
static ULONG64 g_cpuSelectedAddr = 0;  // single-click highlight

// Option / help window state.
static bool g_showExceptions = false;  // Ignored-exceptions page open
static bool g_showCmdHelp = false;     // Command Reference window open
static bool g_showTerminal = false;    // Combined terminal (log + command input)
static char g_addExcBuf[32] = "";      // add-exception input
static char g_termCmdBuf[512] = "";    // terminal command input

// Built-in command reference, shown in the Command Reference window ("?").
struct CmdHelp { const char* name; const char* help; };
static const CmdHelp kBuiltinCommands[] = {
    { "launch <cmdline>",   "Launch a new target under the debugger" },
    { "attach <pid>",       "Attach to a running process" },
    { "g",                  "Go / run" },
    { "pause",              "Break into the running target" },
    { "t",                  "Step into" },
    { "p",                  "Step over" },
    { "rtr",                "Run to return (step out)" },
    { "bp <mod!sym|addr>",  "Set a breakpoint" },
    { "bc <id>",            "Clear breakpoint by id" },
    { "u [addr]",           "Disassemble at addr (or RIP)" },
    { "d/db/dw/dd [addr]",  "Dump memory (byte/word/dword)" },
    { "eb <addr> <hex>",    "Edit bytes at addr" },
    { "r [reg] [val]",      "Show / read / set registers" },
    { "kill",               "Terminate the target and end the session" },
    { "restart",            "Kill and re-run the same target from the top" },
    { "proc <id>",          "Switch the panes to another debugged process" },
    { "closeproc <id>",     "Drop an exited process's tab from the CPU window" },
    { "childdbg [on|off]",  "Debug child processes (follow spawned processes)" },
    { "breakmod [on|off]",  "Break on every new module (DLL) load" },
    { "breakthread [on|off]","Break on every new thread" },
    { "ignoreexc <hex>",    "Pass an exception code straight to the debuggee" },
    { "catchexc <hex>",     "Stop ignoring an exception code" },
    { "exceptions / sx",    "List currently ignored exception codes" },
};

// Plugin-registered commands (Odbg_RegisterCommand), provided by host_exports.cpp.
std::vector<std::pair<std::string, std::string>> HostGetPluginCommands();

struct ProcEntry { DWORD pid; std::string name; };
static std::vector<ProcEntry> g_procList;

static void RefreshProcessList() {
    g_procList.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            int n = WideCharToMultiByte(CP_ACP, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
            std::string a(n ? n - 1 : 0, '\0');
            if (n) WideCharToMultiByte(CP_ACP, 0, pe.szExeFile, -1, &a[0], n, nullptr, nullptr);
            g_procList.push_back({ pe.th32ProcessID, a });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    std::sort(g_procList.begin(), g_procList.end(),
        [](const ProcEntry& a, const ProcEntry& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
}

// Opens the native file-open dialog; returns true and fills buf if the user
// picked a file.
static bool BrowseForExecutable(char* buf, size_t bufSize) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)IM_ARRAYSIZE(file);
    ofn.lpstrTitle = L"Select executable to debug";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    int n = WideCharToMultiByte(CP_ACP, 0, file, -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_ACP, 0, file, -1, &a[0], n, nullptr, nullptr);
    strncpy_s(buf, bufSize, a.c_str(), _TRUNCATE);
    return true;
}

// Pushes a command built from a printf-style format, for the many places that
// need "verb <hex address>".
static void PushCmdF(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    std::wstring wcmd;
    for (char c : std::string(buf)) wcmd += (wchar_t)c;
    g_session->PushCommand(wcmd);
}

static void DrawMenuAndToolbar(const Snapshot& snap) {
    // Menus hold configuration/session-management actions; every live debug
    // control (run/pause/step) lives in the toolbar below, Olly-style.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Debug")) {
            if (ImGui::MenuItem("Open executable...")) { g_showOpenPopup = true; }
            if (ImGui::MenuItem("Attach to process...")) { g_showAttachPopup = true; }
            ImGui::Separator();
            if (ImGui::MenuItem("Restart", "Ctrl+F2")) g_session->PushCommand(L"restart");
            if (ImGui::MenuItem("Kill process", nullptr, false, snap.sessionActive))
                g_session->PushCommand(L"kill");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::BeginMenu("Theme")) {
                for (int i = 0; i < (int)IM_ARRAYSIZE(kThemes); i++) {
                    bool sel = (g_themeIndex == i);
                    if (ImGui::MenuItem(kThemes[i].name, nullptr, sel)) g_themeIndex = i;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem("odbg-terminal", nullptr, &g_showTerminal);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            // Toggles are driven through the command dispatcher (so the engine
            // worker owns the actual state change); the snapshot reflects it back.
            Snapshot s = g_session->GetSnapshot();
            bool child = s.optChildDbg, bmod = s.optBreakModule, bthr = s.optBreakThread;
            if (ImGui::MenuItem("Debug child processes", nullptr, &child))
                g_session->PushCommand(child ? L"childdbg on" : L"childdbg off");
            if (ImGui::MenuItem("Break on new module (DLL)", nullptr, &bmod))
                g_session->PushCommand(bmod ? L"breakmod on" : L"breakmod off");
            if (ImGui::MenuItem("Break on new thread", nullptr, &bthr))
                g_session->PushCommand(bthr ? L"breakthread on" : L"breakthread off");
            ImGui::Separator();
            // Horizontal scrolling is no longer an option here - each pane has
            // its own toggle in its header.
            ImGui::MenuItem("Remember last opened executable", nullptr, &g_rememberLastExe);
            ImGui::Separator();
            if (ImGui::MenuItem("Ignored exceptions...")) g_showExceptions = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Command reference...")) g_showCmdHelp = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Plugins")) {
            PluginManager& plugins = g_session->Plugins();
            if (plugins.Count() == 0) {
                ImGui::TextDisabled("(none loaded - drop DLLs in plugins/)");
            } else {
                for (size_t i = 0; i < plugins.Count(); i++) {
                    if (ImGui::BeginMenu(plugins.Name(i).c_str())) {
                        const auto& items = plugins.MenuItems(i);
                        if (items.empty()) ImGui::TextDisabled("(no menu items)");
                        for (size_t a = 0; a < items.size(); a++) {
                            if (ImGui::MenuItem(items[a].c_str())) plugins.FireAction(i, (int)a);
                        }
                        ImGui::EndMenu();
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const Theme& th = CurTheme();
    ImGui::Spacing();
    if (ToolbarIconButton("open", "Open executable...", th.text, IconOpen)) g_showOpenPopup = true;
    ImGui::SameLine();
    if (ToolbarIconButton("attach", "Attach to process...", th.text, IconAttach)) g_showAttachPopup = true;
    ImGui::SameLine();
    if (ToolbarIconButton("restart", "Restart current target (Ctrl+F2)",
                          ImVec4(0.55f, 0.80f, 0.95f, 1.0f), IconRestart))
        g_session->PushCommand(L"restart");
    // A gap before the kill button: it is the one destructive control up here,
    // and it should not sit flush against restart where a slipped click lands.
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3.0f);
    ImGui::BeginDisabled(!snap.sessionActive);
    if (ToolbarIconButton("kill", "Kill process", ImVec4(0.95f, 0.35f, 0.35f, 1.0f), IconKill))
        g_session->PushCommand(L"kill");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ToolbarSeparator();
    if (ToolbarIconButton("run", "Run (F9)", ImVec4(0.35f, 0.85f, 0.35f, 1.0f), IconRun)) g_session->PushCommand(L"g");
    ImGui::SameLine();
    if (ToolbarIconButton("pause", "Pause", ImVec4(0.95f, 0.75f, 0.25f, 1.0f), IconPause)) g_session->PushCommand(L"pause");
    ImGui::SameLine();
    ToolbarSeparator();
    if (ToolbarIconButton("stepinto", "Step into (F7)", ImVec4(0.35f, 0.80f, 0.95f, 1.0f), IconStepInto)) g_session->PushCommand(L"t");
    ImGui::SameLine();
    if (ToolbarIconButton("stepover", "Step over (F8)", ImVec4(0.55f, 0.65f, 0.95f, 1.0f), IconStepOver)) g_session->PushCommand(L"p");
    ImGui::SameLine();
    if (ToolbarIconButton("ret", "Execute till return (Ctrl+F9)", ImVec4(0.80f, 0.55f, 0.90f, 1.0f), IconRet)) g_session->PushCommand(L"rtr");
    ImGui::Spacing();
}

static void DrawPopups() {
    if (g_showOpenPopup) { ImGui::OpenPopup("Open executable"); g_showOpenPopup = false; }
    if (ImGui::BeginPopupModal("Open executable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Command line:");
        ImGui::SetNextItemWidth(440);
        ImGui::InputText("##launchpath", g_launchBuf, sizeof(g_launchBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) BrowseForExecutable(g_launchBuf, sizeof(g_launchBuf));
        if (ImGui::Button("Launch") && g_launchBuf[0]) {
            std::string cmd = "launch ";
            cmd += g_launchBuf;
            int wlen = MultiByteToWideChar(CP_ACP, 0, cmd.c_str(), -1, nullptr, 0);
            std::wstring wcmd(wlen ? wlen - 1 : 0, L'\0');
            if (wlen) MultiByteToWideChar(CP_ACP, 0, cmd.c_str(), -1, &wcmd[0], wlen);
            g_session->PushCommand(wcmd);
            // Remember the target now rather than at exit, so it survives odbg
            // being killed along with a misbehaving debuggee.
            strncpy_s(g_lastExe, g_launchBuf, _TRUNCATE);
            if (g_rememberLastExe) {
                g_settings.Set("LastExe", g_lastExe);
                g_settings.Save(SettingsPath());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_showAttachPopup) {
        ImGui::OpenPopup("Attach to process");
        g_showAttachPopup = false;
        g_attachSelectedPid = 0;
        RefreshProcessList();
    }
    if (ImGui::BeginPopupModal("Attach to process", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##attachfilter", "Filter by name...", g_attachFilter, sizeof(g_attachFilter));
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) RefreshProcessList();

        ImGui::BeginChild("##proclist", ImVec2(420, 320), true);
        std::string filter = g_attachFilter;
        for (auto& c : filter) c = (char)tolower((unsigned char)c);
        for (const auto& p : g_procList) {
            if (!filter.empty()) {
                std::string nameLower = p.name;
                for (auto& c : nameLower) c = (char)tolower((unsigned char)c);
                if (nameLower.find(filter) == std::string::npos) continue;
            }
            char label[300];
            sprintf_s(label, "%6u  %s", p.pid, p.name.c_str());
            if (ImGui::Selectable(label, g_attachSelectedPid == p.pid)) g_attachSelectedPid = p.pid;
        }
        ImGui::EndChild();

        ImGui::BeginDisabled(g_attachSelectedPid == 0);
        if (ImGui::Button("Attach")) {
            std::wstring wcmd = L"attach " + std::to_wstring(g_attachSelectedPid);
            g_session->PushCommand(wcmd);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Ignored-exceptions page (OllyDbg's Options > Exceptions) and the Command
// Reference window (the "?" button). Both are plain floating windows toggled
// from the menus / command bar.
static void DrawHelpAndOptionWindows(const Snapshot& snap) {
    if (g_showExceptions) {
        ImGui::SetNextWindowSize(ImVec2(400, 440), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Ignored exceptions", &g_showExceptions)) {
            ImGui::TextWrapped("First-chance exception codes passed straight to the "
                               "debuggee instead of breaking (OllyDbg-style). Add a "
                               "single code or an inclusive range LO-HI.");
            ImGui::Separator();

            ImGui::TextDisabled("Ignored:");
            ImGui::BeginChild("##exclist", ImVec2(0, 140), true);
            for (size_t i = 0; i < snap.ignoredExceptions.size(); i++) {
                const ExcRange& e = snap.ignoredExceptions[i];
                if (e.lo == e.hi) ImGui::Text("0x%08lX", e.lo);
                else              ImGui::Text("0x%08lX - 0x%08lX", e.lo, e.hi);
                ImGui::SameLine(210);
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("Remove")) {
                    wchar_t cmd[32]; swprintf_s(cmd, L"rmexc %zu", i);
                    g_session->PushCommand(cmd);
                }
                ImGui::PopID();
            }
            if (snap.ignoredExceptions.empty()) ImGui::TextDisabled("(none)");
            ImGui::EndChild();

            ImGui::SetNextItemWidth(180);
            // Hex digits plus '-' for ranges; CharsHexadecimal alone rejects '-'.
            ImGui::InputTextWithHint("##addexc", "e.g. c0000005 or 40000000-4000ffff",
                                     g_addExcBuf, sizeof(g_addExcBuf));
            ImGui::SameLine();
            if (ImGui::Button("Ignore") && g_addExcBuf[0]) {
                std::wstring wcmd = L"ignoreexc ";
                for (char c : std::string(g_addExcBuf)) wcmd += (wchar_t)c;
                g_session->PushCommand(wcmd);
                g_addExcBuf[0] = 0;
            }

            ImGui::Separator();
            ImGui::TextDisabled("Seen this session (click to ignore):");
            ImGui::BeginChild("##seenlist", ImVec2(0, 0), true);
            if (snap.seenExceptions.empty()) ImGui::TextDisabled("(none yet)");
            for (unsigned long code : snap.seenExceptions) {
                bool already = false;
                for (const auto& e : snap.ignoredExceptions)
                    if (code >= e.lo && code <= e.hi) { already = true; break; }
                ImGui::Text("0x%08lX", code);
                ImGui::SameLine(210);
                ImGui::PushID((int)code + 0x10000);
                if (already) {
                    ImGui::TextDisabled("ignored");
                } else if (ImGui::SmallButton("Ignore")) {
                    wchar_t cmd[32]; swprintf_s(cmd, L"ignoreexc %lx", code);
                    g_session->PushCommand(cmd);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    if (g_showCmdHelp) {
        ImGui::SetNextWindowSize(ImVec2(480, 460), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Command reference", &g_showCmdHelp)) {
            ImGui::TextDisabled("Type these in the Command pane.");
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Built-in commands", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("builtins", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 170);
                    ImGui::TableSetupColumn("Description");
                    for (const auto& c : kBuiltinCommands) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.help);
                    }
                    ImGui::EndTable();
                }
            }
            auto pluginCmds = HostGetPluginCommands();
            if (ImGui::CollapsingHeader("Plugin commands", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (pluginCmds.empty()) {
                    ImGui::TextDisabled("(no plugin commands registered)");
                } else if (ImGui::BeginTable("plugincmds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 170);
                    ImGui::TableSetupColumn("Description");
                    for (const auto& c : pluginCmds) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.first.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.second.c_str());
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::End();
    }
}

// Terminal window: combined log output + command input, like a real terminal.
// Auto-scrolls to bottom on new output. Enter submits the command.
static void DrawTerminal(const Snapshot& snap, const Theme& th) {
    if (!g_showTerminal) return;
    ImGui::SetNextWindowSize(ImVec2(640, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("odbg-terminal", &g_showTerminal)) {
        PaneScrollToggle("term", &g_scrollTerm, th);
        // Log output area (fills remaining space).
        float cmdH = ImGui::GetFrameHeightWithSpacing() * 1.5f;
        ImGui::BeginChild("##termlog", ImVec2(0, -cmdH), true,
            g_scrollTerm ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        for (const auto& line : snap.log) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (!snap.log.empty()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        // Command input at the bottom, Olly-style prompt.
        ImGui::Separator();
        ImGui::Text(">");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##termcmd", g_termCmdBuf, sizeof(g_termCmdBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (g_termCmdBuf[0]) {
                std::wstring wcmd;
                for (char c : std::string(g_termCmdBuf)) wcmd += (wchar_t)c;
                g_session->PushCommand(wcmd);
                g_termCmdBuf[0] = 0;
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// CPU window. One tab per process in the session (child-following can give you
// several at once), each tab flagged live or dead; inside, the disassembly is
// a list of live rows - click to select, double-click to toggle a breakpoint,
// right-click for the rest.
// ---------------------------------------------------------------------------

static const BpInfo* BreakpointAt(const Snapshot& snap, ULONG64 addr) {
    for (const auto& b : snap.breakpoints) if (b.addr == addr) return &b;
    return nullptr;
}

// Double-clicking a row sets a breakpoint; double-clicking one that already has
// a breakpoint clears it. Both go through the command dispatcher, so the engine
// worker owns the change (and it shows up in the log like a typed command).
static void ToggleBreakpointAt(const Snapshot& snap, ULONG64 addr) {
    const BpInfo* bp = BreakpointAt(snap, addr);
    if (bp) PushCmdF("bc %u", bp->id);
    else    PushCmdF("bp %llx", (unsigned long long)addr);
}

static void DrawDisasmRow(const Snapshot& snap, const DisasmLine& dl, const Theme& th, int digits) {
    const BpInfo* bp = BreakpointAt(snap, dl.addr);
    bool isCurrent = snap.stopped && dl.addr == snap.regs.rip;
    bool isSelected = dl.addr == g_cpuSelectedAddr;

    ImGui::PushID((int)(dl.addr & 0xFFFFFFFF));

    float lineH = ImGui::GetTextLineHeight();
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + lineH);
    ImDrawList* dl2 = ImGui::GetWindowDrawList();

    // Row background, in priority order: a breakpoint always reads red, even
    // when it is also the current instruction or the selected row.
    if (bp)           dl2->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(th.breakpointRow));
    else if (isCurrent) dl2->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(th.currentLineBg));
    else if (isSelected) dl2->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(th.selectedRow));
    // ...so a selected breakpoint/current row is marked with an outline instead.
    if (isSelected && (bp || isCurrent))
        dl2->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(th.text), 0.0f, 0, 1.0f);

    // An invisible full-width hit target under the text: the text itself is
    // drawn afterwards, on top, so it stays coloured per token.
    bool clicked = ImGui::Selectable("##row", false,
        ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, lineH));
    if (clicked) {
        g_cpuSelectedAddr = dl.addr;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ToggleBreakpointAt(snap, dl.addr);
    }

    if (ImGui::BeginPopupContextItem("##rowmenu")) {
        g_cpuSelectedAddr = dl.addr;
        char addrText[32];
        FormatAddr(addrText, sizeof(addrText), dl.addr, digits);
        ImGui::TextDisabled("%s", addrText);
        ImGui::Separator();
        if (!bp) {
            if (ImGui::MenuItem("Set breakpoint", "dbl-click"))
                PushCmdF("bp %llx", (unsigned long long)dl.addr);
        } else {
            char label[64];
            sprintf_s(label, "Remove breakpoint %u", bp->id);
            if (ImGui::MenuItem(label, "dbl-click")) PushCmdF("bc %u", bp->id);
        }
        if (ImGui::MenuItem("Run to here", nullptr, false, snap.stopped)) {
            // Olly's "run to selection": arm it, go, and leave it set - clearing
            // it on hit would need engine-side one-shot support.
            PushCmdF("bp %llx", (unsigned long long)dl.addr);
            g_session->PushCommand(L"g");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Follow in Dump")) PushCmdF("d %llx", (unsigned long long)dl.addr);
        if (ImGui::MenuItem("Set RIP here", nullptr, false, snap.stopped)) {
            PushCmdF("r rip %llx", (unsigned long long)dl.addr);
            PushCmdF("u %llx", (unsigned long long)dl.addr);
        }
        if (ImGui::MenuItem("Go to RIP", nullptr, false, snap.stopped)) g_session->PushCommand(L"u rip");
        ImGui::Separator();
        if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(addrText);
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(rowMin);
    DrawDisasmText(dl, th, digits);

    ImGui::PopID();
}

static void DrawCpuDisasm(const Snapshot& snap, const Theme& th) {
    if (!snap.sessionActive) {
        ImGui::TextDisabled("No process. Debug > Open executable... or Attach to process...");
        return;
    }
    if (snap.disasmLines.empty()) {
        ImGui::TextDisabled("(no disassembly at this address)");
        return;
    }
    ULONG64 maxAddr = 0;
    for (const auto& dl : snap.disasmLines) maxAddr = dl.addr > maxAddr ? dl.addr : maxAddr;
    int digits = AddrDigitsFor(maxAddr);
    for (const auto& dl : snap.disasmLines) DrawDisasmRow(snap, dl, th, digits);
}

// A live process gets a play triangle on its tab, a dead one an X. Both are
// drawn over the tab after it is submitted, into the gap the label's leading
// spaces reserve - the mono font has no glyphs for either shape.
static void DrawTabStateGlyph(bool alive, const Theme& th) {
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    float h = mx.y - mn.y;
    float s = h * 0.32f;
    float cx = mn.x + ImGui::GetStyle().FramePadding.x + s;
    float cy = (mn.y + mx.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (alive) {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.85f, 0.35f, 1.0f));
        dl->AddTriangleFilled(ImVec2(cx - s * 0.7f, cy - s), ImVec2(cx - s * 0.7f, cy + s),
                              ImVec2(cx + s * 0.9f, cy), col);
    } else {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), col, 1.8f);
        dl->AddLine(ImVec2(cx + s, cy - s), ImVec2(cx - s, cy + s), col, 1.8f);
    }
    (void)th;
}

static void DrawCpuWindow(const Snapshot& snap, const Theme& th) {
    ImGui::Begin("CPU", nullptr, g_scrollCpu ? ImGuiWindowFlags_HorizontalScrollbar : 0);
    PaneScrollToggle("cpu", &g_scrollCpu, th);

    if (snap.processes.empty()) {
        DrawCpuDisasm(snap, th);
        ImGui::End();
        return;
    }

    // A process the session has never shown before (the target itself, or a
    // child it just spawned) pulls the tab selection to itself - once, when it
    // appears, not on every frame afterwards.
    ULONG snapTo = 0;
    if (snap.procSerial != g_seenProcSerial) {
        g_seenProcSerial = snap.procSerial;
        snapTo = snap.focusProcEngineId;
        g_cpuTabProc = snapTo;
    }

    if (ImGui::BeginTabBar("##cputabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (const auto& p : snap.processes) {
            char label[128];
            // Leading spaces reserve room for the live/dead glyph; ### keeps the
            // tab's identity stable even though the visible text can change.
            sprintf_s(label, "    %s (%u)###proc%u", p.name.c_str(), p.pid, p.engineId);

            ImGuiTabItemFlags flags = (snapTo && snapTo == p.engineId) ? ImGuiTabItemFlags_SetSelected : 0;
            bool keepOpen = true;
            // Only a dead tab gets a close button - a live process is closed by
            // killing it, not by hiding it.
            bool selected = ImGui::BeginTabItem(label, p.alive ? nullptr : &keepOpen, flags);
            DrawTabStateGlyph(p.alive, th);
            if (!keepOpen) PushCmdF("closeproc %u", p.engineId);

            if (selected) {
                g_cpuTabProc = p.engineId;
                // Whichever tab is showing - clicked, or snapped to because the
                // debuggee just spawned it - is the process the engine should be
                // pointed at, so the registers/stack/dump follow the tab. Sent
                // once per selection: the snapshot only catches up a frame or
                // more later, and re-sending until then would spam the queue.
                bool needSwitch = p.alive && snap.stopped && p.engineId != snap.currentProcEngineId;
                if (needSwitch && g_procRequested != p.engineId) {
                    g_procRequested = p.engineId;
                    PushCmdF("proc %u", p.engineId);
                }
                if (p.engineId == snap.currentProcEngineId) g_procRequested = 0;

                if (!p.alive) {
                    ImGui::TextDisabled("Process %u exited. Close this tab with the x on it.", p.pid);
                } else if (p.engineId != snap.currentProcEngineId) {
                    ImGui::TextDisabled(snap.stopped ? "Switching to this process..."
                                                     : "Pause the target to inspect this process.");
                } else {
                    DrawCpuDisasm(snap, th);
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

int main(int, char**) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(CrashHandler);
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    g_settings.Load(SettingsPath());
    int savedTheme = g_settings.GetInt("Theme", 0);
    g_themeIndex = savedTheme >= 0 && savedTheme < (int)IM_ARRAYSIZE(kThemes) ? savedTheme : 0;
    int winX = g_settings.GetInt("X", 100), winY = g_settings.GetInt("Y", 100);
    int savedW = g_settings.GetInt("W", 0), savedH = g_settings.GetInt("H", 0);
    int winW = savedW > 0 ? savedW : (int)(1400 * scale);
    int winH = savedH > 0 ? savedH : (int)(900 * scale);

    // Per-pane scroll toggles. "DumpHScroll" is the old global option's key -
    // read it as the Dump pane's starting value so an existing settings file
    // carries over instead of silently resetting.
    g_scrollCpu   = g_settings.GetBool("ScrollCpu", false);
    g_scrollDump  = g_settings.GetBool("ScrollDump", g_settings.GetBool("DumpHScroll", false));
    g_scrollStack = g_settings.GetBool("ScrollStack", false);
    g_scrollRegs  = g_settings.GetBool("ScrollRegs", false);
    g_scrollLog   = g_settings.GetBool("ScrollLog", false);
    g_scrollTerm  = g_settings.GetBool("ScrollTerminal", true);
    g_showTerminal = g_settings.GetBool("ShowTerminal", false);
    g_rememberLastExe = g_settings.GetBool("RememberLastExe", true);
    if (g_rememberLastExe) {
        strncpy_s(g_lastExe, g_settings.Get("LastExe").c_str(), _TRUNCATE);
        // Pre-fill the Open dialog with it, so reopening the last target is
        // Debug > Open > Launch with nothing to retype.
        strncpy_s(g_launchBuf, g_lastExe, _TRUNCATE);
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"odbg", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"odbg - open-debugger",
        WS_OVERLAPPEDWINDOW, winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Persist the dock/pane/splitter (border) layout next to the exe, so it
    // survives relaunch regardless of the working directory we started in.
    static std::string imguiIniPath = ExeSiblingPath(L"odbg_imgui.ini");
    io.IniFilename = imguiIniPath.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts = true;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImFont* monoFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f * scale);
    if (!monoFont) monoFont = io.Fonts->AddFontDefault();

    DbgSession session;
    g_session = &session;

    // Re-apply persisted options so they come back "as options turned on". These
    // go through the command dispatcher on the worker thread, same as a user
    // toggle. Order doesn't matter; the worker drains them before the first stop.
    if (g_settings.GetBool("ChildDbg"))    session.PushCommand(L"childdbg on");
    if (g_settings.GetBool("BreakModule")) session.PushCommand(L"breakmod on");
    if (g_settings.GetBool("BreakThread")) session.PushCommand(L"breakthread on");
    {
        std::string ignored = g_settings.Get("IgnoredExc");
        std::string cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            std::wstring wcmd = L"ignoreexc ";
            for (char c : cur) wcmd += (wchar_t)c;
            session.PushCommand(wcmd);
            cur.clear();
        };
        for (char c : ignored) { if (c == ',') flush(); else if (c != ' ') cur += c; }
        flush();
    }

    bool done = false;
    static char cmdBuf[256] = "";
    RegFile prevRegs{};

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(monoFont, 0.0f);

        Snapshot snap = session.GetSnapshot();
        const Theme& th = CurTheme();

        // Global hotkeys: mirror Olly's F7/F8/F9, active regardless of which
        // pane has focus.
        if (snap.sessionActive) {
            if (ImGui::IsKeyPressed(ImGuiKey_F9)) session.PushCommand(snap.stopped ? L"g" : L"pause");
            if (snap.stopped && ImGui::IsKeyPressed(ImGuiKey_F7)) session.PushCommand(L"t");
            if (snap.stopped && ImGui::IsKeyPressed(ImGuiKey_F8)) session.PushCommand(L"p");
        }
        // Olly's Ctrl+F2 - and it works with no session too, re-running the
        // last target this instance was given.
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F2)) session.PushCommand(L"restart");

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, th.windowBg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("OSDDockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(2);

        DrawMenuAndToolbar(snap);
        DrawPopups();
        DrawHelpAndOptionWindows(snap);
        DrawTerminal(snap, th);

        ImGuiID dockspaceId = ImGui::GetID("OSDDockSpace");
        // Only lay out the default split the very first time this dockspace
        // has ever existed; once imgui.ini has a saved node, respect however
        // the user last arranged the panes instead of resetting it every launch.
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            BuildDockLayout(dockspaceId, viewport->WorkSize);
        }
        float statusBarH = ImGui::GetFrameHeightWithSpacing();
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -statusBarH), ImGuiDockNodeFlags_None);

        // Bottom status bar, Olly-style: session state, current instruction,
        // last event/command result.
        ImGui::Separator();
        if (!snap.sessionActive) {
            ImGui::TextDisabled("No process");
        } else {
            ImGui::TextColored(snap.stopped ? ImVec4(0.95f, 0.75f, 0.25f, 1.0f) : ImVec4(0.35f, 0.85f, 0.35f, 1.0f),
                "%s", snap.stopped ? "Paused" : "Running");
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            char ripText[32];
            FormatAddr(ripText, sizeof(ripText), snap.regs.rip, 16);
            ImGui::Text("RIP %s", ripText);
            ImGui::SameLine();
            ImGui::TextUnformatted("|");
            ImGui::SameLine();
            if (!snap.log.empty()) ImGui::TextUnformatted(snap.log.back().c_str());
        }
        ImGui::End(); // OSDDockHost
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_WindowBg, th.paneBg);

        DrawCpuWindow(snap, th);

        ImGui::Begin("Registers", nullptr, g_scrollRegs ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        PaneScrollToggle("regs", &g_scrollRegs, th);
        if (!snap.sessionActive) {
            ImGui::TextDisabled("-");
        } else {
            auto row = [&](const char* name, ULONG64 val, ULONG64 prevVal) {
                bool changed = val != prevVal;
                char valText[32];
                FormatAddr(valText, sizeof(valText), val, 16);
                ImGui::PushStyleColor(ImGuiCol_Text, changed ? th.changedReg : th.text);
                ImGui::Text("%-4s %s", name, valText);
                ImGui::PopStyleColor();
            };
            row("RAX", snap.regs.rax, prevRegs.rax); row("RBX", snap.regs.rbx, prevRegs.rbx);
            row("RCX", snap.regs.rcx, prevRegs.rcx); row("RDX", snap.regs.rdx, prevRegs.rdx);
            row("RSI", snap.regs.rsi, prevRegs.rsi); row("RDI", snap.regs.rdi, prevRegs.rdi);
            row("RBP", snap.regs.rbp, prevRegs.rbp); row("RSP", snap.regs.rsp, prevRegs.rsp);
            row("RIP", snap.regs.rip, prevRegs.rip);
            row("R8 ", snap.regs.r8,  prevRegs.r8);  row("R9 ", snap.regs.r9,  prevRegs.r9);
            row("R10", snap.regs.r10, prevRegs.r10); row("R11", snap.regs.r11, prevRegs.r11);
            prevRegs = snap.regs;
        }
        ImGui::End();

        ImGui::Begin("Dump", nullptr, g_scrollDump ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        PaneScrollToggle("dump", &g_scrollDump, th);
        if (snap.dumpBytes.empty()) {
            ImGui::TextDisabled("-");
        } else {
            int dumpDigits = AddrDigitsFor(snap.dumpViewAddr + snap.dumpBytes.size());
            for (size_t row = 0; row * 16 < snap.dumpBytes.size(); row++) {
                size_t base = row * 16;
                size_t n = std::min<size_t>(16, snap.dumpBytes.size() - base);
                char rowAddr[32];
                FormatAddr(rowAddr, sizeof(rowAddr), snap.dumpViewAddr + base, dumpDigits);
                ImGui::PushStyleColor(ImGuiCol_Text, th.addr);
                ImGui::TextUnformatted(rowAddr);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                std::string hex, ascii;
                char b[4];
                for (size_t i = 0; i < n; i++) {
                    sprintf_s(b, "%02X ", snap.dumpBytes[base + i]);
                    hex += b;
                    unsigned char c = snap.dumpBytes[base + i];
                    ascii += (c >= 32 && c < 127) ? (char)c : '.';
                }
                ImGui::PushStyleColor(ImGuiCol_Text, th.bytes);
                ImGui::TextUnformatted(hex.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, th.text);
                ImGui::TextUnformatted(ascii.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();

        ImGui::Begin("Stack", nullptr, g_scrollStack ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        PaneScrollToggle("stack", &g_scrollStack, th);
        if (snap.stack.empty()) {
            ImGui::TextDisabled("-");
        } else {
            ULONG64 maxStackAddr = 0;
            for (const auto& e : snap.stack) {
                maxStackAddr = e.addr > maxStackAddr ? e.addr : maxStackAddr;
                maxStackAddr = e.value > maxStackAddr ? e.value : maxStackAddr;
            }
            int stackDigits = AddrDigitsFor(maxStackAddr);
            for (const auto& e : snap.stack) {
                char buf[32];
                FormatAddr(buf, sizeof(buf), e.addr, stackDigits);
                ImGui::PushStyleColor(ImGuiCol_Text, th.addr);
                ImGui::TextUnformatted(buf);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                FormatAddr(buf, sizeof(buf), e.value, stackDigits);
                ImGui::PushStyleColor(ImGuiCol_Text, th.imm);
                ImGui::TextUnformatted(buf);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                std::string sym;
                for (wchar_t wc : e.symbol) sym += (char)wc;
                ImGui::PushStyleColor(ImGuiCol_Text, th.symbol);
                ImGui::TextUnformatted(sym.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();

        ImGui::Begin("Command");
        float helpBtnW = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(-(helpBtnW + ImGui::GetStyle().ItemSpacing.x));
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
        if (ImGui::InputText("##cmd", cmdBuf, sizeof(cmdBuf), flags)) {
            if (cmdBuf[0]) {
                std::wstring wcmd;
                for (char c : std::string(cmdBuf)) wcmd += (wchar_t)c;
                session.PushCommand(wcmd);
                cmdBuf[0] = 0;
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("?", ImVec2(helpBtnW, 0))) g_showCmdHelp = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Command reference");
        ImGui::End();

        ImGui::Begin("Log", nullptr, g_scrollLog ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        PaneScrollToggle("log", &g_scrollLog, th);
        for (const auto& line : snap.log) ImGui::TextUnformatted(line.c_str());
        if (!snap.log.empty()) ImGui::SetScrollHereY(1.0f);
        ImGui::End();

        ImGui::PopStyleColor(); // paneBg

        ImGui::PopFont();
        ImGui::Render();
        const float clearColor[4] = { 0.06f, 0.06f, 0.06f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        // WM_DESTROY (fired by the loop-ending WM_CLOSE) tears the HWND down
        // before we get back here, so GetWindowRect on it would fail at that
        // point - snapshot the rect each live frame instead and save that.
        if (!::IsIconic(hwnd)) {
            RECT rc;
            if (::GetWindowRect(hwnd, &rc)) {
                g_lastWinRect = rc;
                g_haveWinRect = true;
            }
        }
    }

    {
        // Anything not overwritten here (keys from an older build, plugin
        // settings written through Odbg_Setsetting) is still in the store from
        // Load() and gets written straight back out.
        if (g_haveWinRect) {
            g_settings.SetInt("X", g_lastWinRect.left);
            g_settings.SetInt("Y", g_lastWinRect.top);
            g_settings.SetInt("W", g_lastWinRect.right - g_lastWinRect.left);
            g_settings.SetInt("H", g_lastWinRect.bottom - g_lastWinRect.top);
        }
        g_settings.SetInt("Theme", g_themeIndex);
        Snapshot fin = session.GetSnapshot();
        g_settings.SetBool("ChildDbg", fin.optChildDbg);
        g_settings.SetBool("BreakModule", fin.optBreakModule);
        g_settings.SetBool("BreakThread", fin.optBreakThread);
        g_settings.SetBool("ScrollCpu", g_scrollCpu);
        g_settings.SetBool("ScrollDump", g_scrollDump);
        g_settings.SetBool("ScrollStack", g_scrollStack);
        g_settings.SetBool("ScrollRegs", g_scrollRegs);
        g_settings.SetBool("ScrollLog", g_scrollLog);
        g_settings.SetBool("ScrollTerminal", g_scrollTerm);
        g_settings.SetBool("ShowTerminal", g_showTerminal);
        g_settings.SetBool("RememberLastExe", g_rememberLastExe);
        // Whatever launched last wins, whether it came from the Open dialog
        // (which records into g_lastExe immediately) or from a typed/piped
        // `launch`, which only the session knows about.
        if (!fin.lastTarget.empty()) strncpy_s(g_lastExe, fin.lastTarget.c_str(), _TRUNCATE);
        g_settings.Set("LastExe", g_rememberLastExe ? g_lastExe : "");
        std::string ignored;
        for (size_t i = 0; i < fin.ignoredExceptions.size(); i++) {
            char b[32]; sprintf_s(b, "%lx-%lx", fin.ignoredExceptions[i].lo, fin.ignoredExceptions[i].hi);
            if (i) ignored += ",";
            ignored += b;
        }
        g_settings.Set("IgnoredExc", ignored);
        // Plugins get their last chance to persist state before this write:
        // CloseAll() fires each Odbg_Pluginclose, which may call Odbg_Setsetting.
        session.Plugins().CloseAll();
        g_settings.Save(SettingsPath());
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
    backBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
