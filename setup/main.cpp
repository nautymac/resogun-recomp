// SPDX-License-Identifier: GPL-2.0-or-later
// resogun-setup.exe: sets up the release folder it sits in (tools/package.py) from the player's
// Resogun dump, so the game starts from resogun.exe or a desktop shortcut.
//
// With no arguments it opens a window: the game folder (the dump), the extra DLC folder, the save
// folder, 3D (settings.ini stereo: off, SR panel, side by side), whether to skip the intro logos
// (settings.ini skip_intro), Install and Exit; after an
// install it offers a desktop shortcut. Run it again to change a folder, or after unpacking a newer
// release over the folder.
// resogun.exe opens it when the game is not set up. With --update (resogun.exe, when game/ isn't
// prepared for this release or from the dump's eboot.bin as it is now) it prepares game/ again from
// paths.ini's folders in a small progress window, and exits 0 when the game can start.
// With arguments it installs from the command line:
//
//     resogun-setup.exe --game <dump or eboot.bin> [--save <folder>] [--copy-saves]
//                           [--dlc <folder>|--no-dlc] [--skip-intro|--play-intro] [--progress]
//                           [--stereo off|sr|sbs] [--shortcut <folder>|desktop]

#include "setup.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Resogun setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

// --- command line -------------------------------------------------------------------------------

// Writes to the standard handles a parent gave us, or else to the console of the shell we were started from.
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        err_ = GetStdHandle(STD_ERROR_HANDLE);
        if ((!out_ || !err_) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (!out_)
                out_ = con;
            if (!err_)
                err_ = con;
        }
    }
    void out(const std::wstring& s) const { write(out_, s + L"\n"); }
    void err(const std::wstring& s) const { write(err_, s + L"\n"); }

private:
    static void write(HANDLE h, const std::wstring& s) {
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode, n;
        if (GetConsoleMode(h, &mode)) {
            WriteConsoleW(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), len, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

constexpr wchar_t kUsage[] =
    L"usage: resogun-setup.exe --game <dump or eboot.bin> [--save <folder>] [--copy-saves]\n"
    L"                             [--dlc <folder>|--no-dlc] [--skip-intro|--play-intro] [--progress]\n"
    L"                             [--stereo off|sr|sbs] [--shortcut <folder>|desktop]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, stream.dat); only ever read\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save\\ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --dlc         one more folder of DLC to read, e.g. shadPS4's addcont (default: the one\n"
    L"                paths.ini names); --no-dlc reads only the save folder's addcont\n"
    L"  --skip-intro, --play-intro\n"
    L"                skip the start-up logos and notices, or show them again\n"
    L"                (settings.ini skip_intro; default: leave it as it is)\n"
    L"  --stereo      3D: off, sr (woven for an SR panel; needs ps2sr.dll here) or sbs (side by\n"
    L"                side, for a 3D TV or monitor) (settings.ini stereo; default: leave it as it is)\n"
    L"  --progress    also print \"progress <0 to 1> <step>\" lines (for RecompLauncher)\n"
    L"  --shortcut    also write the game's shortcut into this folder, or onto the Desktop\n"
    L"Sets up the folder this exe is in. With no arguments, opens the setup window; with --update alone,\n"
    L"prepares the game again from the folders already set, in a small window (resogun.exe does this).";

int run_cli(const std::vector<std::wstring>& args) {
    Console con;
    std::wstring game, save, shortcut, dlc;
    bool copy_saves = false, no_dlc = false, progress = false;
    std::optional<bool> skip_intro;
    std::optional<std::wstring> stereo;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                              : a == L"--dlc"      ? &dlc
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--skip-intro" || a == L"--play-intro") {
            skip_intro = a == L"--skip-intro";
        } else if (a == L"--stereo" && i + 1 < args.size()) {
            stereo = args[++i];
            if (*stereo != L"off" && *stereo != L"sr" && *stereo != L"sbs") {
                con.err(std::wstring(kUsage) + L"\n\n--stereo takes off, sr or sbs");
                return 2;
            }
        } else if (a == L"--no-dlc") {
            no_dlc = true;
        } else if (a == L"--progress") {
            progress = true;
        } else {
            con.err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        con.err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = game;
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? fs::path(save) : old ? old->save : setup::default_save();
        opt.dlc = no_dlc ? fs::path() : !dlc.empty() ? fs::path(dlc) : old ? old->dlc : fs::path();
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_intro = skip_intro;
        opt.stereo = stereo;
        if (progress)
            opt.progress = [&](double done, const std::wstring& what) {
                wchar_t fraction[16];
                swprintf(fraction, 16, L"%.2f", done);
                con.out(L"progress " + std::wstring(fraction) + L" " + what);
            };
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { con.out(line); });
        if (!shortcut.empty()) {
            fs::path lnk = (shortcut == L"desktop" ? setup::desktop_folder() : fs::path(shortcut)) / setup::kShortcutName;
            setup::create_shortcut(exe, lnk);
            con.out(L"shortcut " + lnk.wstring());
        }
        con.out(L"\nDone. Run " + exe.wstring());
        return 0;
    } catch (const setup::Error& e) {
        con.err(e.message);
        return 1;
    }
}

// --- window -------------------------------------------------------------------------------------
// Drawn in the colours of the game's Display Settings panel (F10; gpu/shim/resogun_overlay.cpp
// PageInk, as they show on screen): a framed dark-teal panel on deep blue, teal rows, and the
// highlighted cyan for the action that is ready. Install stays disabled until the game folder holds a
// Resogun dump, the DLC folder (optional) exists when one is named, and a save folder is named.

enum : int {
    kGameEdit = 101,
    kGameBrowse,
    kDlcEdit,
    kDlcBrowse,
    kSaveEdit,
    kSaveBrowse,
    kPlay,
    kSkipIntro,
    kStereo, // kStereo + i: setup::kStereoModes[i]
    kStereoLast = kStereo + 2,
    kPage,     // Game settings / Back
    kSizeEdit, // the window size on the Game settings page
};
// The Game settings page's buttons: kSettingBase + row * 16 + the option (or -, +, the right button).
constexpr int kSettingBase = 400;
enum : UINT {
    kMsgLog = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,             // wParam: 1 ok; lParam: std::wstring* exe path or error
};

namespace color {
constexpr COLORREF Back = RGB(0x01, 0x0c, 0x1d), Panel = RGB(0x00, 0x24, 0x38), Frame = RGB(0x5f, 0xc8, 0xd8);
constexpr COLORREF Tab = RGB(0x00, 0x40, 0x57);
constexpr COLORREF RowTop = RGB(0x00, 0x70, 0x8b), RowBottom = RGB(0x00, 0x4c, 0x65), RowEdge = RGB(0x3a, 0x9c, 0xb0);
constexpr COLORREF FocusTop = RGB(0x00, 0xcb, 0xff), FocusBottom = RGB(0x00, 0x57, 0x80), FocusEdge = RGB(0xd8, 0xfa, 0xff);
constexpr COLORREF OffTop = RGB(0x0a, 0x34, 0x44), OffBottom = RGB(0x06, 0x24, 0x30);
constexpr COLORREF Field = RGB(0x00, 0x16, 0x24), Text = RGB(0xff, 0xff, 0xff), Help = RGB(0xb0, 0xe4, 0xec);
constexpr COLORREF Dim = RGB(0x5a, 0x86, 0x92), Good = RGB(0x8c, 0xec, 0xf4), Bad = RGB(0xff, 0xb4, 0x78);
} // namespace color

// One folder row: its label, the field and Browse, and what the check says about the folder.
struct Folder {
    const wchar_t* label;
    int y;
    HWND edit = nullptr, browse = nullptr;
    std::wstring note;
    bool ok = false;
};

struct Ui {
    HWND wnd = nullptr, play = nullptr, install = nullptr, exit = nullptr, skip_intro = nullptr;
    HWND stereo[3] = {}; // the 3D choices (setup::kStereoModes)
    int stereo_mode = 0;
    HWND page_button = nullptr;
    int page = 0; // 0 the install, 1 Game settings
    std::wstring install_status; // the install page's status while Game settings shows
    bool install_status_bad = false;
    Folder game{L"Game folder", 66}, dlc{L"Extra DLC folder (optional)", 146}, save{L"Save folder", 226};
    std::vector<HWND> inputs; // disabled while installing (Play Game too, and until the folder is set up)
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    HBRUSH field = nullptr;
    UINT dpi = 96;
    bool busy = false, skip = true;
    std::wstring status;
    bool status_bad = false;
    fs::path root;
};
Ui ui;

// Layout in 96-dpi units; the client area is kWidth x kHeight.
constexpr int kWidth = 640, kHeight = 532;
constexpr int kRowLeft = 36, kRowRight = 604, kRowHeight = 72, kFieldLeft = 58, kBrowseLeft = 480, kBrowseWidth = 104;
constexpr int kPanelTop = 40, kPanelBottom = 432, kButtonsTop = 484;
constexpr int kStereoY = 306, kIntroY = 390; // the 3D row, the intro box
constexpr const wchar_t* kStereoLabels[] = {L"Off", L"SR panel", L"Side by side"};

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}

RECT rect(int x, int y, int w, int h) {
    return {px(x), px(y), px(x + w), px(y + h)};
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    size_t a = s.find_first_not_of(L" \t\""), b = s.find_last_not_of(L" \t\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// --- drawing ---

// A trapezoid `skew` narrower on each side at the bottom (the game's rows and tabs), filled with a
// vertical gradient and outlined.
void shape(HDC dc, RECT r, int skew, COLORREF top, COLORREF bottom, COLORREF edge, int edge_width = 1) {
    POINT pts[4] = {{r.left, r.top}, {r.right - 1, r.top}, {r.right - 1 - skew, r.bottom - 1}, {r.left + skew, r.bottom - 1}};
    HRGN rgn = CreatePolygonRgn(pts, 4, WINDING);
    SaveDC(dc);
    SelectClipRgn(dc, rgn);
    TRIVERTEX v[2] = {
        {r.left, r.top, static_cast<COLOR16>(GetRValue(top) << 8), static_cast<COLOR16>(GetGValue(top) << 8),
         static_cast<COLOR16>(GetBValue(top) << 8), 0},
        {r.right, r.bottom, static_cast<COLOR16>(GetRValue(bottom) << 8), static_cast<COLOR16>(GetGValue(bottom) << 8),
         static_cast<COLOR16>(GetBValue(bottom) << 8), 0},
    };
    GRADIENT_RECT g = {0, 1};
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
    RestoreDC(dc, -1);
    DeleteObject(rgn);
    HPEN pen = CreatePen(PS_SOLID, edge_width, edge);
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Polygon(dc, pts, 4);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, RECT r, COLORREF c, int width = 1) {
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, c);
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

void text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF c, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

// The box the edit sits in: a dark field with the rows' edge colour.
RECT field_box(const Folder& f) {
    return rect(kFieldLeft, f.y + 36, kBrowseLeft - kFieldLeft - 10, 28);
}

void paint_folder(HDC dc, const Folder& f) {
    shape(dc, rect(kRowLeft, f.y, kRowRight - kRowLeft, kRowHeight), px(10), color::RowTop, color::RowBottom, color::RowEdge);
    text(dc, f.label, rect(kFieldLeft, f.y + 8, 240, 24), ui.bold, color::Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(dc, f.note, rect(kFieldLeft + 240, f.y + 8, kRowRight - 20 - kFieldLeft - 240, 24), ui.font,
         f.ok ? color::Good : color::Bad, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT box = field_box(f);
    fill(dc, box, color::Field);
    frame(dc, box, color::RowEdge);
}

// --- the Game settings page ---
// The game's settings.ini keys (gpu/shim/resogun_settings.cpp) as rows of choices like the 3D row; a
// click saves at once to the files the game reads (setup::set_game_settings). The in-game Display
// Settings (F10) and the 3D keys change the same lines.

struct Option {
    const wchar_t* label;
    const wchar_t* value;
};
struct SettingRow {
    const wchar_t* label;
    const wchar_t* key;          // settings.ini key; vsync's row sets vsync and tearing
    std::vector<Option> options; // none: the window size field, or a 3D number with - and +
    const wchar_t* fallback;     // what the game takes when the key is unset
    std::vector<HWND> buttons;
    int chosen = 0;
};
enum : int {
    kRowMode, kRowAspect, kRowSize, kRowFps, kRowVsync, kRowRender, kRowSharpen, kRowHdr, kRowAudio,
    kRowDepth, kRowConvergence, kSettingRows
};
std::vector<SettingRow> settings = {
    {L"Window mode", L"display_mode", {{L"Windowed", L"windowed"}, {L"Borderless", L"borderless"}, {L"Fullscreen", L"fullscreen"}}, L"borderless"},
    {L"Aspect ratio", L"aspect_ratio", {{L"4:3", L"4:3"}, {L"16:9", L"16:9"}, {L"16:10", L"16:10"}, {L"21:9", L"21:9"}, {L"32:9", L"32:9"}}, L"16:9"},
    {L"Window size", L"output_resolution", {}, L"1920x1080"},
    {L"Max FPS", L"frame_rate", {{L"30", L"30"}, {L"60", L"60"}, {L"90", L"90"}, {L"120", L"120"}, {L"144", L"144"}, {L"180", L"180"}}, L"60"},
    {L"VSync", L"vsync", {{L"On", L"on"}, {L"Off", L"off"}, {L"Off, tearing", L"tearing"}}, L"on"},
    {L"Render resolution", L"render_resolution", {{L"1080p", L"1080p"}, {L"4K", L"4k"}}, L"4k"},
    {L"Sharpening", L"sharpening", {{L"On", L"1"}, {L"Off", L"0"}}, L"1"},
    {L"HDR", L"hdr", {{L"Off", L"0"}, {L"On", L"1"}}, L"0"},
    {L"Speakers", L"audio_channels", {{L"Auto", L"auto"}, {L"Stereo", L"stereo"}, {L"5.1", L"5.1"}, {L"7.1", L"7.1"}}, L"auto"},
    {L"3D depth", L"stereo_depth", {}, L"1"},
    {L"3D convergence", L"stereo_convergence", {}, L"60"},
};
float stereo_depth = 1.0f, stereo_convergence = 60.0f;
bool stereo_swap = false;
HWND size_edit = nullptr;
bool loading_settings = false; // the size field set from the file is not a change to save

// One bar per row, the choices on its right (96-dpi units).
constexpr int kSetTop = 54, kSetPitch = 34, kSetHeight = 31, kChoicesLeft = 214, kChoicesRight = 588;

int setting_id(int row, int index) {
    return kSettingBase + row * 16 + index;
}

bool setting_control(int id, int& row, int& index) {
    if (id < kSettingBase || id >= kSettingBase + kSettingRows * 16)
        return false;
    row = (id - kSettingBase) / 16;
    index = (id - kSettingBase) % 16;
    return true;
}

// The highlighted buttons: each row's choice, and Swap eyes while on.
bool setting_selected(int row, int index) {
    if (row == kRowDepth)
        return index == 2 && stereo_swap;
    if (row == kRowSize || row == kRowConvergence)
        return false;
    return index == settings[row].chosen;
}

// A number as the game writes it: up to three decimals, trailing zeros dropped.
std::wstring number(float v) {
    wchar_t t[32];
    swprintf(t, 32, L"%.3f", v);
    std::wstring s = t;
    s.erase(s.find_last_not_of(L'0') + 1);
    if (!s.empty() && s.back() == L'.')
        s.pop_back();
    return s;
}

void paint_settings(HDC dc) {
    for (int i = 0; i < kSettingRows; ++i) {
        const int y = kSetTop + i * kSetPitch;
        shape(dc, rect(kRowLeft, y, kRowRight - kRowLeft, kSetHeight), px(6), color::RowTop, color::RowBottom,
              color::RowEdge);
        text(dc, settings[i].label, rect(kRowLeft + 18, y, kChoicesLeft - kRowLeft - 22, kSetHeight), ui.bold,
             color::Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (i == kRowSize) {
            RECT box = rect(kChoicesLeft, y + 3, 130, kSetHeight - 6);
            fill(dc, box, color::Field);
            frame(dc, box, color::RowEdge);
            text(dc, L"Windowed only (width x height)", rect(kChoicesLeft + 140, y, kChoicesRight - kChoicesLeft - 140, kSetHeight),
                 ui.font, color::Help, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (i == kRowDepth || i == kRowConvergence) {
            const std::wstring value = i == kRowDepth ? number(stereo_depth) + L" %" : number(stereo_convergence);
            text(dc, value, rect(kChoicesLeft + 36, y, 72, kSetHeight), ui.bold, color::Text,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// The install page: the three folders and the 3D row.
void paint_install(HDC dc) {
    for (const Folder* f : {&ui.game, &ui.dlc, &ui.save})
        paint_folder(dc, *f);
    // The 3D row: its choice buttons and what the chosen one does.
    shape(dc, rect(kRowLeft, kStereoY, kRowRight - kRowLeft, kRowHeight), px(10), color::RowTop, color::RowBottom,
          color::RowEdge);
    text(dc, L"3D (stereo)", rect(kFieldLeft, kStereoY + 8, 240, 24), ui.bold, color::Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::error_code ec;
    const bool no_bridge = ui.stereo_mode == 1 && !fs::is_regular_file(ui.root / L"ps2sr.dll", ec);
    const wchar_t* note = ui.stereo_mode == 0   ? L"2D"
                          : no_bridge           ? L"No ps2sr.dll here: side by side instead"
                          : ui.stereo_mode == 1 ? L"Woven for an SR panel (SpatialLabs)"
                                                : L"For a 3D TV or monitor in SBS mode";
    text(dc, note, rect(kFieldLeft + 240, kStereoY + 8, kRowRight - 20 - kFieldLeft - 240, 24), ui.font,
         no_bridge ? color::Bad : color::Good, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paint(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    fill(dc, client, color::Back);
    // The panel and its title tab, as the Display Settings page draws them.
    RECT panel = rect(20, kPanelTop, kWidth - 40, kPanelBottom - kPanelTop);
    fill(dc, panel, color::Panel);
    frame(dc, panel, color::Frame, px(2));
    RECT tab = rect(kWidth / 2 - 140, 12, 280, 42);
    shape(dc, tab, px(12), color::Tab, color::Panel, color::Frame, px(2));
    text(dc, ui.page == 1 ? L"GAME SETTINGS" : L"RESOGUN SETUP", tab, ui.title, color::Text,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (ui.page == 1) {
        paint_settings(dc);
    } else {
        paint_install(dc);
    }
    text(dc, ui.status, rect(24, kPanelBottom + 10, kWidth - 48, kButtonsTop - kPanelBottom - 16), ui.font,
         ui.status_bad ? color::Bad : color::Help, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Owner-drawn buttons: the rows' teal, the highlighted cyan for the one action that is ready, and a
// dim teal when disabled.
void draw_button(const DRAWITEMSTRUCT& d) {
    HDC dc = d.hDC;
    RECT r = d.rcItem;
    int setting_row = 0, setting_index = 0;
    const bool setting = setting_control(int(d.CtlID), setting_row, setting_index);
    const bool choice = (d.CtlID >= kStereo && d.CtlID <= kStereoLast) || setting; // on a row's gradient
    fill(dc, r, d.hwndItem == ui.skip_intro ? color::Panel : d.CtlID == kGameBrowse || d.CtlID == kDlcBrowse ||
                                                                 d.CtlID == kSaveBrowse || choice
                                                             ? color::RowBottom
                                                             : color::Back);
    bool disabled = d.itemState & ODS_DISABLED, pressed = d.itemState & ODS_SELECTED, focus = d.itemState & ODS_FOCUS;
    wchar_t label[128] = {};
    GetWindowTextW(d.hwndItem, label, 128);
    if (d.hwndItem == ui.skip_intro) {
        RECT box = {r.left + px(2), r.top + (r.bottom - r.top - px(18)) / 2, r.left + px(20), 0};
        box.bottom = box.top + px(18);
        fill(dc, box, color::Field);
        frame(dc, box, focus ? color::FocusEdge : color::RowEdge, focus ? px(2) : 1);
        if (ui.skip) {
            HPEN pen = CreatePen(PS_SOLID, px(2), color::Good);
            HGDIOBJ old = SelectObject(dc, pen);
            POINT check[3] = {{box.left + px(4), box.top + px(9)}, {box.left + px(8), box.top + px(13)},
                              {box.left + px(14), box.top + px(5)}};
            Polyline(dc, check, 3);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        RECT t = {box.right + px(10), r.top, r.right, r.bottom};
        text(dc, label, t, ui.font, disabled ? color::Dim : color::Help, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    bool primary = setting ? setting_selected(setting_row, setting_index)
                   : choice  ? int(d.CtlID) - kStereo == ui.stereo_mode
                   : d.hwndItem == ui.page_button
                       ? false
                       : d.hwndItem == (setup::ready(ui.root) && !IsWindowEnabled(ui.install) ? ui.play : ui.install);
    COLORREF top = disabled ? color::OffTop : primary ? color::FocusTop : color::RowTop;
    COLORREF bottom = disabled ? color::OffBottom : primary ? color::FocusBottom : color::RowBottom;
    if (pressed)
        std::swap(top, bottom);
    bool browse = d.CtlID == kGameBrowse || d.CtlID == kDlcBrowse || d.CtlID == kSaveBrowse || choice;
    int skew = browse ? 0 : (r.bottom - r.top) / 4; // Browse sits on a row's gradient: no corners to fill
    shape(dc, r, skew, top, bottom, focus ? color::FocusEdge : disabled ? color::Dim : color::RowEdge,
          focus ? px(2) : 1);
    text(dc, label, r, ui.bold, disabled ? color::Dim : color::Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// --- state ---

void set_status(std::wstring s, bool bad = false) {
    ui.status = std::move(s);
    ui.status_bad = bad;
    RECT r = rect(0, kPanelBottom + 4, kWidth, kButtonsTop - kPanelBottom - 6);
    InvalidateRect(ui.wnd, &r, FALSE);
}

// Checks the three folders, shows the result in each row and enables Install when all are usable.
void validate() {
    std::error_code ec;
    std::wstring game = text_of(ui.game.edit), dlc = text_of(ui.dlc.edit), save = text_of(ui.save.edit);
    ui.game.ok = false;
    if (game.empty()) {
        ui.game.note = L"Required: your decrypted dump";
    } else if (!fs::is_directory(setup::game_folder(game), ec)) {
        ui.game.note = L"Folder not found";
    } else if (auto missing = setup::missing_game_files(game); !missing.empty()) {
        ui.game.note = L"Not a Resogun dump: no " + missing.front();
    } else {
        ui.game.ok = true;
        ui.game.note = L"Resogun dump found";
    }
    ui.dlc.ok = dlc.empty() || fs::is_directory(fs::path(dlc), ec);
    ui.dlc.note = dlc.empty() ? L"Optional: none chosen" : ui.dlc.ok ? L"Folder found" : L"Folder not found";
    ui.save.ok = !save.empty();
    ui.save.note = !ui.save.ok                         ? L"Required"
                   : fs::is_directory(fs::path(save), ec) ? L"Folder found"
                                                        : L"Created when you install";
    EnableWindow(ui.install, !ui.busy && ui.game.ok && ui.dlc.ok && ui.save.ok);
    InvalidateRect(ui.wnd, nullptr, FALSE);
    InvalidateRect(ui.install, nullptr, FALSE);
    InvalidateRect(ui.play, nullptr, FALSE);
}

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    EnableWindow(ui.play, !busy && setup::ready(ui.root));
    validate();
}

// --- the Game settings page: reading, saving, switching ---

fs::path settings_save() {
    std::wstring s = text_of(ui.save.edit);
    return s.empty() ? setup::default_save() : fs::path(s);
}

std::wstring lower_text(std::wstring s) {
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool truthy(const std::wstring& v) {
    const std::wstring l = lower_text(v);
    return l == L"1" || l == L"true" || l == L"yes" || l == L"on";
}

// The rows from the settings.ini the game reads (the save folder named on the install page).
void load_settings() {
    const fs::path save = settings_save();
    auto get = [&](const wchar_t* key, const wchar_t* fallback) {
        std::optional<std::wstring> v = setup::game_setting(ui.root, save, key);
        return lower_text(v && !v->empty() ? *v : std::wstring(fallback));
    };
    for (int i = 0; i < kSettingRows; ++i) {
        SettingRow& row = settings[i];
        if (row.options.empty())
            continue;
        std::wstring v;
        if (i == kRowVsync) {
            v = truthy(get(L"vsync", L"1")) ? L"on" : truthy(get(L"tearing", L"0")) ? L"tearing" : L"off";
        } else {
            v = get(row.key, row.fallback);
            if (i == kRowSharpen || i == kRowHdr)
                v = truthy(v) ? L"1" : L"0";
            if (i == kRowRender && v == L"2160p")
                v = L"4k";
            if (i == kRowAudio)
                v = v == L"2.0" || v == L"2" ? L"stereo" : v == L"6" ? L"5.1" : v == L"8" ? L"7.1" : v;
        }
        int found = -1, fallback = 0;
        for (size_t o = 0; o < row.options.size(); ++o) {
            if (v == row.options[o].value)
                found = int(o);
            if (lower_text(row.fallback) == row.options[o].value)
                fallback = int(o);
        }
        row.chosen = found >= 0 ? found : fallback; // an unknown value: the game takes its default too
    }
    const float depth = float(_wtof(get(L"stereo_depth", L"1").c_str()));
    const float convergence = float(_wtof(get(L"stereo_convergence", L"60").c_str()));
    stereo_depth = std::clamp(depth, 0.0f, 10.0f);
    stereo_convergence = convergence > 0.0f ? std::clamp(convergence, 0.01f, 100000.0f) : 60.0f;
    stereo_swap = truthy(get(L"stereo_swap", L"0"));
    loading_settings = true;
    SetWindowTextW(size_edit, get(L"output_resolution", L"1920x1080").c_str());
    loading_settings = false;
    for (const SettingRow& row : settings)
        for (HWND b : row.buttons)
            InvalidateRect(b, nullptr, FALSE);
}

void save_settings(const std::vector<std::pair<std::wstring, std::wstring>>& values) {
    try {
        setup::set_game_settings(ui.root, settings_save(), values);
        std::wstring what;
        for (const auto& [key, value] : values)
            what += (what.empty() ? L"" : L", ") + key + L"=" + value;
        set_status(L"Saved in settings.ini: " + what);
    } catch (const setup::Error& e) {
        set_status(e.message, true);
    }
}

void choose_setting(int row, int index) {
    if (row == kRowDepth || row == kRowConvergence) {
        if (row == kRowDepth && index < 2)
            stereo_depth = std::clamp(std::round((stereo_depth + (index ? 0.1f : -0.1f)) * 100.0f) / 100.0f, 0.0f, 10.0f);
        if (row == kRowConvergence && index < 2)
            stereo_convergence = std::clamp(index ? stereo_convergence * 1.1f : stereo_convergence / 1.1f, 0.01f, 100000.0f);
        if (row == kRowDepth && index == 2)
            stereo_swap = !stereo_swap;
        if (row == kRowConvergence && index == 2) { // Defaults
            stereo_depth = 1.0f;
            stereo_convergence = 60.0f;
        }
        RECT area = rect(kRowLeft, kSetTop + kRowDepth * kSetPitch, kRowRight - kRowLeft, 2 * kSetPitch);
        InvalidateRect(ui.wnd, &area, FALSE);
        for (HWND b : settings[kRowDepth].buttons)
            InvalidateRect(b, nullptr, FALSE);
        save_settings({{L"stereo_depth", number(stereo_depth)},
                       {L"stereo_convergence", number(stereo_convergence)},
                       {L"stereo_swap", stereo_swap ? L"1" : L"0"}});
        return;
    }
    SettingRow& r = settings[row];
    if (index < 0 || index >= int(r.options.size()))
        return;
    r.chosen = index;
    for (HWND b : r.buttons)
        InvalidateRect(b, nullptr, FALSE);
    if (row == kRowVsync) {
        const std::wstring v = r.options[index].value;
        save_settings({{L"vsync", v == L"on" ? L"1" : L"0"}, {L"tearing", v == L"tearing" ? L"1" : L"0"}});
    } else {
        save_settings({{r.key, r.options[index].value}});
    }
}

// The window size field: saved once it reads as a size.
void size_changed() {
    if (loading_settings)
        return;
    const std::wstring s = text_of(size_edit);
    int w = 0, h = 0;
    wchar_t x = 0;
    if (swscanf(s.c_str(), L"%d%lc%d", &w, &x, &h) == 3 && (x == L'x' || x == L'X') && w >= 640 && h >= 360 &&
        w <= 15360 && h <= 8640)
        save_settings({{L"output_resolution", std::to_wstring(w) + L"x" + std::to_wstring(h)}});
    else
        set_status(L"Window size: width x height, as 1920x1080.", true);
}

void show_page(int page) {
    if (page == ui.page)
        return;
    if (page == 1) {
        ui.install_status = ui.status;
        ui.install_status_bad = ui.status_bad;
        load_settings();
    }
    ui.page = page;
    const int install = page == 0 ? SW_SHOW : SW_HIDE, game = page == 1 ? SW_SHOW : SW_HIDE;
    for (const Folder* f : {&ui.game, &ui.dlc, &ui.save}) {
        ShowWindow(f->edit, install);
        ShowWindow(f->browse, install);
    }
    for (HWND h : ui.stereo)
        ShowWindow(h, install);
    ShowWindow(ui.skip_intro, install);
    ShowWindow(ui.install, install);
    for (const SettingRow& row : settings)
        for (HWND b : row.buttons)
            ShowWindow(b, game);
    ShowWindow(size_edit, game);
    SetWindowTextW(ui.page_button, page == 0 ? L"Game settings" : L"Back");
    if (page == 1)
        set_status(L"Each choice is saved in settings.ini at once; the game reads it when it starts. "
                   L"3D renders at 1080p in 16:9 whatever these say.");
    else
        set_status(ui.install_status, ui.install_status_bad);
    InvalidateRect(ui.wnd, nullptr, FALSE);
}

// Starts the game as a double-click would (its own folder, no console), then closes the setup.
void play() {
    fs::path exe = ui.root / L"resogun.exe";
    auto started = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ui.wnd, L"open", exe.c_str(), nullptr, ui.root.c_str(), SW_SHOWNORMAL));
    if (started <= 32) {
        MessageBoxW(ui.wnd, (L"Cannot start " + exe.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    DestroyWindow(ui.wnd);
}

// The shell's folder picker, starting in the folder the field names when it exists.
void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring current = text_of(edit);
    if (!current.empty()) {
        std::error_code ec;
        fs::path start = setup::game_folder(current);
        IShellItem* item = nullptr;
        if (fs::is_directory(start, ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path); // EN_CHANGE validates it
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
}

// Game files that are not the verified build: a warning box over `wnd`, from the install's thread
// (MessageBoxW runs its own message loop while the window waits). No is the default.
bool confirm_untested(HWND wnd, const std::wstring& warning) {
    return MessageBoxW(wnd, warning.c_str(), kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void start_install() {
    if (ui.busy || !IsWindowEnabled(ui.install))
        return;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit), dlc = text_of(ui.dlc.edit);
    setup::Options opt;
    opt.root = ui.root;
    opt.game = game;
    opt.save = save;
    opt.dlc = dlc;
    opt.skip_intro = ui.skip;
    opt.stereo = setup::kStereoModes[ui.stereo_mode];
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            std::wstring ask = L"Copy your saves to the new save folder?\n\nFrom: " + old->wstring() + L"\nTo: " + save;
            int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                opt.copy_saves_from = old;
        }
    } catch (const setup::Error& e) {
        MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Installing...");
    set_busy(true);
    std::thread([opt, wnd = ui.wnd]() mutable {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        opt.confirm = [wnd](const std::wstring& warning) { return confirm_untested(wnd, warning); };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
}

void finish_install(bool ok, const std::wstring& value) {
    set_busy(false);
    if (!ok) {
        set_status(L"Install failed. Check the folders and press Install again.", true);
        MessageBoxW(ui.wnd, value.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Done. Press Play Game to start.");
    if (MessageBoxW(ui.wnd, L"Done!\n\nAdd a shortcut to the game on your desktop?", kTitle,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        try {
            setup::create_shortcut(value, setup::desktop_folder() / setup::kShortcutName);
            set_status(L"Done, with a desktop shortcut. Press Play Game to start.");
        } catch (const setup::Error& e) {
            MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    SetFocus(ui.play); // Enter now starts the game
}

// --- controls ---

HWND add(const wchar_t* cls, const wchar_t* label, DWORD style, RECT r, int id) {
    HWND c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, ui.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    return c;
}

void add_folder(Folder& f, int edit_id, int browse_id) {
    RECT box = field_box(f);
    // The edit inside its drawn box, its text centred on the box's height.
    int h = px(20);
    RECT r = {box.left + px(7), box.top + (box.bottom - box.top - h) / 2, box.right - px(7), 0};
    r.bottom = r.top + h;
    f.edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, r, edit_id);
    f.browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_OWNERDRAW, rect(kBrowseLeft, f.y + 35, kBrowseWidth, 30),
                   browse_id);
}

void create_controls() {
    // Tab order: the three folders top to bottom, the intro box, then the buttons.
    add_folder(ui.game, kGameEdit, kGameBrowse);
    add_folder(ui.dlc, kDlcEdit, kDlcBrowse);
    add_folder(ui.save, kSaveEdit, kSaveBrowse);
    // The 3D choices, side by side across the row's field.
    constexpr int kChoiceGap = 8;
    const int choice_width = (kRowRight - 22 - kFieldLeft - 2 * kChoiceGap) / 3;
    for (int i = 0; i < 3; ++i)
        ui.stereo[i] = add(WC_BUTTONW, kStereoLabels[i], WS_TABSTOP | BS_OWNERDRAW,
                           rect(kFieldLeft + i * (choice_width + kChoiceGap), kStereoY + 35, choice_width, 30),
                           kStereo + i);
    ui.skip_intro = add(WC_BUTTONW, L"Skip the intro logos (skip_intro=0 in settings.ini brings them back)",
                        WS_TABSTOP | BS_OWNERDRAW, rect(kRowLeft + 4, kIntroY, kRowRight - kRowLeft - 8, 30), kSkipIntro);
    ui.play = add(WC_BUTTONW, L"Play Game", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 28 - 3 * 128 + 8, kButtonsTop, 120, 36), kPlay);
    ui.install = add(WC_BUTTONW, L"Install", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 28 - 2 * 128 + 8, kButtonsTop, 120, 36), IDOK);
    ui.exit = add(WC_BUTTONW, L"Exit", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 28 - 128 + 8, kButtonsTop, 120, 36), IDCANCEL);
    ui.page_button = add(WC_BUTTONW, L"Game settings", WS_TABSTOP | BS_OWNERDRAW, rect(28, kButtonsTop, 150, 36), kPage);
    ui.inputs = {ui.game.edit, ui.game.browse, ui.dlc.edit, ui.dlc.browse, ui.save.edit, ui.save.browse,
                 ui.stereo[0], ui.stereo[1], ui.stereo[2], ui.skip_intro, ui.exit, ui.page_button};
    // The Game settings page (hidden until its button): the choices, the size field, - and +.
    for (int i = 0; i < kSettingRows; ++i) {
        SettingRow& row = settings[i];
        const int y = kSetTop + i * kSetPitch + 3, h = kSetHeight - 6;
        if (!row.options.empty()) {
            const int n = static_cast<int>(row.options.size()), gap = 6;
            const int w = (kChoicesRight - kChoicesLeft - (n - 1) * gap) / n;
            for (int o = 0; o < n; ++o)
                row.buttons.push_back(add(WC_BUTTONW, row.options[o].label, WS_TABSTOP | BS_OWNERDRAW,
                                          rect(kChoicesLeft + o * (w + gap), y, w, h), setting_id(i, o)));
        } else if (i == kRowDepth || i == kRowConvergence) {
            row.buttons.push_back(add(WC_BUTTONW, L"\u2212", WS_TABSTOP | BS_OWNERDRAW, rect(kChoicesLeft, y, 34, h), setting_id(i, 0)));
            row.buttons.push_back(add(WC_BUTTONW, L"+", WS_TABSTOP | BS_OWNERDRAW, rect(kChoicesLeft + 110, y, 34, h), setting_id(i, 1)));
            row.buttons.push_back(add(WC_BUTTONW, i == kRowDepth ? L"Swap eyes" : L"Defaults", WS_TABSTOP | BS_OWNERDRAW,
                                      rect(kChoicesRight - 130, y, 130, h), setting_id(i, 2)));
        }
        for (HWND b : row.buttons) {
            ShowWindow(b, SW_HIDE);
            ui.inputs.push_back(b);
        }
    }
    {
        RECT box = rect(kChoicesLeft, kSetTop + kRowSize * kSetPitch + 3, 130, kSetHeight - 6);
        const int h = px(19);
        RECT r = {box.left + px(6), box.top + (box.bottom - box.top - h) / 2, box.right - px(6), 0};
        r.bottom = r.top + h;
        size_edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, r, kSizeEdit);
        ShowWindow(size_edit, SW_HIDE);
        ui.inputs.push_back(size_edit);
    }

    // Start from what the last install chose (paths.ini here, moved from an older per-user copy), else an empty
    // game and DLC folder and save\ here.
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    fs::path save = paths ? paths->save : setup::default_save();
    if (paths) {
        SetWindowTextW(ui.game.edit, paths->game.c_str());
        SetWindowTextW(ui.dlc.edit, paths->dlc.c_str());
    }
    SetWindowTextW(ui.save.edit, save.c_str());
    // What settings.ini says now; Install writes the box's state, so unticking it brings the logos back.
    ui.skip = setup::skip_intro(ui.root, save);
    ui.stereo_mode = setup::stereo_mode(ui.root, save); // Install writes the chosen one
    set_status(setup::ready(ui.root) ? L"The game is set up. Press Play Game, or change the folders and press Install."
               : paths              ? L"This version isn't set up yet. Press Install."
                                    : L"Choose your game dump (and a DLC folder if you have one), then press Install.");
    set_busy(false);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) {
            if (LOWORD(wp) == kSizeEdit)
                size_changed();
            else
                validate();
            return 0;
        }
        if (int row = 0, index = 0; setting_control(LOWORD(wp), row, index)) {
            choose_setting(row, index);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDOK:
            start_install();
            return 0;
        case kPlay:
            play();
            return 0;
        case kPage:
            show_page(1 - ui.page);
            return 0;
        case IDCANCEL:
            if (!ui.busy)
                DestroyWindow(wnd);
            return 0;
        case kSkipIntro:
            ui.skip = !ui.skip;
            InvalidateRect(ui.skip_intro, nullptr, FALSE);
            return 0;
        case kStereo:
        case kStereo + 1:
        case kStereo + 2: {
            const int mode = LOWORD(wp) - kStereo;
            if (mode != ui.stereo_mode) {
                ui.stereo_mode = mode;
                for (HWND h : ui.stereo)
                    InvalidateRect(h, nullptr, FALSE);
                RECT row = rect(kRowLeft, kStereoY, kRowRight - kRowLeft, 34);
                InvalidateRect(wnd, &row, FALSE);
            }
            // Saved at once, like the Game settings page: Play Game right after it starts in this mode
            // (Install writes it as well).
            save_settings({{L"stereo", setup::kStereoModes[mode]}});
            return 0;
        }
        case kGameBrowse:
            browse(ui.game.edit, L"Select the game folder (the one with eboot.bin)");
            return 0;
        case kDlcBrowse:
            browse(ui.dlc.edit, L"Select your DLC folder (e.g. shadPS4's addcont)");
            return 0;
        case kSaveBrowse:
            browse(ui.save.edit, L"Select the save folder");
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: { // a disabled edit asks with WM_CTLCOLORSTATIC
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? color::Text : color::Dim);
        SetBkColor(dc, color::Field);
        return reinterpret_cast<LRESULT>(ui.field);
    }
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_ERASEBKGND:
        return 1; // WM_PAINT draws everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT client;
        GetClientRect(wnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        set_status(*line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        finish_install(wp == 1, *value);
        return 0;
    }
    case WM_CLOSE:
        if (!ui.busy)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

HFONT make_font(const LOGFONTW& base, int height, int weight) {
    LOGFONTW lf = base;
    lf.lfHeight = -px(height);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

int run_window(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    ui.font = make_font(ncm.lfMessageFont, 15, FW_NORMAL);
    ui.bold = make_font(ncm.lfMessageFont, 15, FW_SEMIBOLD);
    ui.title = make_font(ncm.lfMessageFont, 22, FW_SEMIBOLD);
    ui.field = CreateSolidBrush(color::Field);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"resogun_setup";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // A dark title bar in the panel's background colour (Windows 11; older versions ignore it).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::Back;
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    create_controls();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.game.edit);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg)) // Tab between controls, Enter = Install, Esc = Exit
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    DeleteObject(ui.bold);
    DeleteObject(ui.title);
    DeleteObject(ui.field);
    return 0;
}

// --- --update -----------------------------------------------------------------------------------
// resogun.exe found game/ not prepared for this release or from the dump's eboot.bin as it is now:
// prepare it again from paths.ini's folders, in a small window of the same look (the title, the
// step, a cyan bar). install() checks the files first; when they aren't the unmodified CUSA00038
// update 1.10 and the player has not gone on with this dump before, a warning asks whether to.

enum : UINT {
    kMsgProgress = WM_APP + 3, // wParam: done in thousandths; lParam: std::wstring* step
};

struct Update {
    double done = 0;
    std::wstring step = L"Checking your game files";
    int result = -1; // the exit code once install() has ended
};
Update update;
constexpr int kUpdateWidth = 480, kUpdateHeight = 132;

void paint_update(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    fill(dc, client, color::Back);
    RECT panel = rect(10, 10, kUpdateWidth - 20, kUpdateHeight - 20);
    fill(dc, panel, color::Panel);
    frame(dc, panel, color::Frame, px(2));
    text(dc, L"UPDATING RESOGUN", rect(28, 18, kUpdateWidth - 56, 32), ui.title, color::Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(dc, update.step, rect(28, 52, kUpdateWidth - 56, 22), ui.font, color::Help,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT bar = rect(28, 82, kUpdateWidth - 56, 18);
    fill(dc, bar, color::Field);
    RECT filled = bar;
    filled.right = bar.left + static_cast<LONG>((bar.right - bar.left) * std::clamp(update.done, 0.0, 1.0));
    if (filled.right - filled.left > 2)
        shape(dc, filled, 0, color::FocusTop, color::FocusBottom, color::FocusTop);
    frame(dc, bar, color::RowEdge);
}

LRESULT CALLBACK update_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT client;
        GetClientRect(wnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_update(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgProgress: {
        std::unique_ptr<std::wstring> step(reinterpret_cast<std::wstring*>(lp));
        update.done = static_cast<double>(wp) / 1000;
        update.step = *step;
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    }
    case kMsgLog:
        delete reinterpret_cast<std::wstring*>(lp);
        return 0;
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        update.result = wp == 1 ? 0 : 1;
        if (wp != 1)
            MessageBoxW(wnd, value->c_str(), kTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(wnd);
        return 0;
    }
    case WM_CLOSE:
        return 0; // until install() ends: closing would leave game/ half prepared
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int run_update(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    if (!paths) {
        MessageBoxW(nullptr, L"The game is not set up yet. Run resogun-setup.exe.", kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    setup::Options opt;
    opt.root = ui.root;
    opt.game = paths->game;
    opt.save = paths->save;
    opt.dlc = paths->dlc;

    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    ui.font = make_font(ncm.lfMessageFont, 15, FW_NORMAL);
    ui.title = make_font(ncm.lfMessageFont, 22, FW_SEMIBOLD);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = update_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"resogun_update";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX;
    RECT r = {0, 0, px(kUpdateWidth), px(kUpdateHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = r.right - r.left, h = r.bottom - r.top;
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, work.left + (work.right - work.left - w) / 2,
                             work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::Back;
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    ShowWindow(ui.wnd, SW_SHOW);
    SetForegroundWindow(ui.wnd);

    std::thread([opt, wnd = ui.wnd]() mutable {
        opt.progress = [wnd](double done, const std::wstring& step) {
            PostMessageW(wnd, kMsgProgress, static_cast<WPARAM>(std::clamp(done, 0.0, 1.0) * 1000),
                         reinterpret_cast<LPARAM>(new std::wstring(step)));
        };
        opt.confirm = [wnd](const std::wstring& warning) { return confirm_untested(wnd, warning); };
        try {
            fs::path exe = setup::install(opt, [](const std::wstring&) {});
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    DeleteObject(ui.title);
    return update.result < 0 ? 1 : update.result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (args.size() == 1 && args[0] == L"--update")
        return run_update(inst);
    if (!args.empty())
        return run_cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
