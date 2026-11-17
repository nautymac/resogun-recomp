// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The install itself, in the release folder the setup exe sits in (tools/package.py): prepares the game
// image from the player's dump with prepare/run.py, and writes paths.ini (the dump, the save folder and
// an optional extra DLC folder, read by src/launcher.c). The dump is only ever read. Windows (main.cpp) and Linux (main_linux.cpp)
// share it.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace setup {

// Why an install stopped; the text is meant for the user.
struct Error {
    std::wstring message;
};

struct Options {
    std::filesystem::path root; // the release folder: the game exe, prepare/, and what the setup adds
    std::filesystem::path game; // the dump folder or its eboot.bin
    std::filesystem::path save; // saves and the launch log (run.py's --user)
    std::optional<std::filesystem::path> copy_saves_from; // the old save folder, whose saves go to `save`
    std::optional<bool> skip_intro; // settings.ini skip_intro to write; unset keeps what is there
    // settings.ini stereo to write (stereo 3D: off, sr or sbs, see kStereoModes); unset keeps it
    std::optional<std::wstring> stereo;
    std::filesystem::path dlc; // optional: one more folder of DLC (e.g. shadPS4's addcont); empty for none
    // Optional: called as each step starts, with how far along the install is (0 to 1) and the step.
    std::function<void(double, const std::wstring&)> progress;
    // Optional: asked, with a warning for the player, whether to go on with game files that are not the
    // verified build (once per dump: game/untested.txt remembers the answer); true goes on. Unset: the
    // warning is logged and the install goes on.
    std::function<bool(const std::wstring&)> confirm;
};

// The folders a release folder's paths.ini names.
struct Paths {
    std::filesystem::path game, save;
    std::filesystem::path dlc; // empty when paths.ini names none
};

// Save data in a save folder (what the runtime keeps there apart from the log).
constexpr const wchar_t* kSaveFolders[] = {L"savedata"};

using Log = std::function<void(const std::wstring&)>;

#ifdef _WIN32
constexpr wchar_t kGameExe[] = L"resogun.exe";
constexpr wchar_t kShortcutName[] = L"Resogun.lnk";
#else
constexpr wchar_t kGameExe[] = L"resogun";
// A desktop entry (freedesktop.org), in the application menu or on the Desktop.
constexpr wchar_t kShortcutName[] = L"resogun.desktop";
#endif

// A path as text for the user and paths.ini, and text back as a path. Linux paths are UTF-8 bytes,
// which these convert without depending on the locale.
std::wstring text(const std::filesystem::path& p);
std::filesystem::path path_of(const std::wstring& s);
// UTF-8 to and from the wide text this setup uses.
std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);

// The folder this exe is in.
std::filesystem::path exe_folder();
// The dump folder for a path to it or to its eboot.bin (absolute, normalized).
std::filesystem::path game_folder(const std::filesystem::path& path);
// The files a Resogun dump needs that `game` (a path game_folder() accepts) lacks; empty for a dump.
std::vector<std::wstring> missing_game_files(const std::filesystem::path& game);
// What `root`/paths.ini names, if it names both folders.
std::optional<Paths> read_paths(const std::filesystem::path& root);
// The per-user copy of paths.ini that setups before v0.12 also wrote: %APPDATA%\Resogun\paths.ini,
// or $XDG_CONFIG_HOME/resogun/paths.ini (~/.config when unset). Empty when %APPDATA% is unset.
std::filesystem::path user_paths_ini();
// Moves that per-user copy into `root` (when `root` has no paths.ini; otherwise it is only deleted),
// and its folder too when nothing else is in it: paths.ini lives only next to the exe. src/launcher.c
// does the same.
void adopt_user_paths(const std::filesystem::path& root);
// adopt_user_paths(root), then read_paths(root): the folders to start from.
std::optional<Paths> remembered_paths(const std::filesystem::path& root);
// save/ in the release folder: a release is never zipped with one, so unpacking a newer release over
// the folder keeps the saves.
std::filesystem::path default_save();
// True when `root` is set up for its release: paths.ini names both folders and game/ was prepared
// for this release.txt from the eboot.bin the dump holds now (what src/launcher.c checks before it
// starts the game; otherwise it has the setup prepare it again with --update).
bool ready(const std::filesystem::path& root);
// game/source.txt's stamp of the dump an image was prepared from: "<size> <mtime>" of its eboot.bin
// (seconds since 1970), as src/launcher.c computes it; empty when there is no eboot.bin.
std::string source_stamp(const std::filesystem::path& game);
// True when `save` holds game saves (a savedata folder with something in it).
bool has_saves(const std::filesystem::path& save);
// settings.ini skip_intro as the game reads it: from the save folder's file when there is one,
// else the release folder's; false when unset.
bool skip_intro(const std::filesystem::path& root, const std::filesystem::path& save);
// Stereo 3D (settings.ini stereo, gpu/shim/resogun_settings.h): off, woven for an SR panel (sr, needs
// ps2sr.dll next to the game exe) or side by side (sbs).
constexpr const wchar_t* kStereoModes[] = {L"off", L"sr", L"sbs"};
// settings.ini stereo as the game reads it (the save folder's file, else the release folder's), as
// an index into kStereoModes; 0 (off) when unset.
int stereo_mode(const std::filesystem::path& root, const std::filesystem::path& save);
// A settings.ini key as the game reads it: from the save folder's file when there is one, else the
// release folder's; unset when that file lacks it.
std::optional<std::wstring> game_setting(const std::filesystem::path& root, const std::filesystem::path& save,
                                         const std::wstring& key);
// Writes settings.ini keys into the files the game reads: the save folder's and the release folder's
// (those that exist; the release folder's is made from prepare/settings.example.ini when neither
// does). Throws Error.
void set_game_settings(const std::filesystem::path& root, const std::filesystem::path& save,
                       const std::vector<std::pair<std::wstring, std::wstring>>& values);
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

// Sets up opt.root, reporting each step through `log`. Throws Error, also when opt.confirm turns down a
// dump that is not the unmodified CUSA00038 update 1.10. Returns the game exe's path.
std::filesystem::path install(const Options& opt, const Log& log);

// Writes a shortcut at `lnk` that starts `target` in its own folder. Throws Error.
void create_shortcut(const std::filesystem::path& target, const std::filesystem::path& lnk);
// The user's Desktop folder. Throws Error.
std::filesystem::path desktop_folder();
#ifndef _WIN32
// Where desktop entries go to show up in the application menu (and Steam's "Add a Non-Steam Game").
std::filesystem::path applications_folder();
#endif

} // namespace setup
