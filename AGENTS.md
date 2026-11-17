# AGENTS.md

Audience: AI coding agents and contributors working on T6MVM.
Read `README.md` first. It is the user-facing contract: features, installation,
console variables, and known runtime caveats. This file is the engineering
contract: structure, build constraints, conventions, and things that must not
be made worse.

## 0. Short version

- T6MVM is a **32-bit Windows plugin DLL** for Call of Duty: Black Ops II
  Redacted (`t6mpv43` and `t6zmv41`). It is injected into the game and hooks
  game and rendering code.
- It is built with **Visual Studio 2022 / MSVC v143**, **C++17**, **Win32
  (x86)**, **MultiByte**, and the Windows 10 SDK. There is no Linux build, no
  test suite, and no package manager. CI only compiles the plugin; it cannot
  run the game.
- The real engine API lives in the external
  [T6SDK](https://github.com/Amyst82/T6SDK) checkout plus `T6SDK.lib`. Never
  invent T6SDK members, game addresses, struct layouts, or event names.
- Make the smallest change that solves the task. Do not reformat, rename,
  modernize, or refactor unrelated code.
- Never add build output, IDE state, `ReleaseDebug/`, `~AutoRecover*`, logs,
  local config, or new binaries. Some of these are already tracked; see
  [Known debt](#8-known-debt-do-not-copy-these-patterns).
- Do not claim a build or runtime test you did not actually perform. A
  source-only review is not a build.

## 1. What this repository is

T6MVM is a theater/demo editing mod. Its feature areas are:

- Demo browsing, demo metadata/bookmarks, timeline seeking.
- Advanced dolly camera, free camera, bone camera, camera shake.
- Custom lights, fog, sun/sky, SSAO, DOF, green screen, Z-depth.
- Multi-pass stream recording (PNG/JPG sequences or AVI, optional camera data).
- Weapon camo and animation replacement, holdgun.
- Optional FFmpeg H.264 MP4 encoding for recorded stream passes.
- Custom sliders, settings, and CFG import/export.

The built plugin is installed under the game's `Plugins` directory. The
shipping `Release|Win32` configuration deliberately produces
`t6mvm.Red32n`, not a normal `.dll`.

The mod does not run standalone. It requires the game at runtime and the
T6SDK headers/library at build time. The startup path sleeps for five seconds
before initializing T6SDK; do not remove that unconditionally without
understanding the host loader.

## 2. Repository map

Everything hand-written for the mod is under `t6mvm/`. Root-level directories
`MinHook/` and `libgmavi/` are vendored dependencies.

| Area | Main files | Responsibility |
|---|---|---|
| Entry point / lifecycle | `t6mvm/dllmain.cpp`, `pch.h`, `pch.cpp`, `framework.h` | DLL entry, worker thread, T6SDK pre-init, `OnGameLoaded` orchestration. |
| Root UI / event hub | `t6mvm/UIBase.h`, `t6mvm/TheaterBase.h` | Registers the end-frame overlay, tabs, key handling, theater events, and per-frame feature updates. |
| Dvars | `t6mvm/CustomDvars.h` | Registration and storage of every T6MVM dvar. |
| Settings | `t6mvm/Settings.h`, `Settings.cpp`, `SettingsMenu.h` | JSON settings, keybinds, directories, post-stream actions. |
| UI primitives | `t6mvm/UIControls.h`, `UI_Timeline.h`, `UI_DemoBrowseButton.h`, `UI_StreamPass.h` | Project-specific widgets; all reusable widgets are declared in `UIControls.h`. |
| Feature tabs | `t6mvm/CameraMenu.h`, `BoneCameraMenu.h`, `MiscMenu.h`, `SsaoMenu.h`, `FogMenu.h`, `DofMenu.h`, `SunMenu.h`, `LightsMenu.h`, `StreamsMenu.h`, `WeaponMenu.h`, `CustomSlidersMenu.h`, `SettingsMenu.h`, `MainMenu.h`, `DemoBrowserMenu.h` | One `Draw()` and usually one `Init()` per tab. |
| Camera | `t6mvm/DollyCamera.h`, `FreeCamera.h`, `BoneCamera.h` | Dolly interpolation, camera shake, frozen camera, free-roam input, bone attachment. |
| Environment / look | `t6mvm/Lights.h`, `LightsListBoxControl.h`, `Fog.h`, `SunMenu.h`, `SsaoMenu.h`, `DofMenu.h`, `Misc.h` | Scene lighting, fog, sun/sky, SSAO, DOF, green screen, depth, misc patches. |
| Demos | `t6mvm/Common.h`, `DemoBrowserMenu.h`, `UI_Timeline.h`, `UI_DemoBrowseButton.h`, `MainMenu.h` | Demo scan/cache, `.tags` metadata, bookmarks, cfg load/save, timeline. |
| Weapons | `t6mvm/Weapon.h`, `WeaponMenu.h`, `WeaponAnimations.h`, `WeaponAnimationChangerMenu.h`, `WeapsListBoxControl.h`, `WeapAnimListBoxControl.h`, `HoldGun.h` | Camo, sway, animation replacement, corpse holdgun. |
| Streams | `t6mvm/Streams.h`, `StreamsCommon.h`, `IMVMStream.h`, `StreamsMenu.h`, `FFmpegEncoder.h`, `Default.h`, `GreenScreen.h`, `NoPlayers.h`, `NoGun.h`, `Depth.h`, `DepthWithGun.h`, `FxOnly.h`, `GreenSky.h` | DX11 capture, AVI/WIC/FFmpeg output, pass registry, tick hook, progress UI. |
| Utilities | `t6mvm/Shake.h`, `Shake.cpp`, `LocalAddresses.h` | Noise/shake helper; project-specific addresses and patches. |
| Windows-only / vendored | `ScreenGrab.*`, `kiero.*`, `wintoastlib.*`, `WinToastHelper.h` | Microsoft WIC screenshot helper, D3D hook helper, WinToast. Do not refactor. |
| Build files | `t6mvm.sln`, `t6mvm/t6mvm.vcxproj`, `t6mvm/t6mvm.vcxproj.filters`, `t6mvm/BuildCounter.ps1` | MSVC solution/project. Keep file lists in sync. |
| Legacy | `t6mvm/AviFile.cpp`, `t6mvm/AviFile.h` | Old AVI writer. Not in the project; no current callers. Do not revive casually. |

`t6mvm/ReleaseDebug/**` and `t6mvm/~AutoRecover.t6mvm.vcxproj*` are tracked
build/IDE debris, not source. Do not copy them, extend them, or use them as
examples. See [Known debt](#8-known-debt-do-not-copy-these-patterns).

### Header-only architecture

Almost all feature code is implemented directly in headers. The project only
compiles these translation units:

- `dllmain.cpp` (pulls in nearly every feature header)
- `pch.cpp`
- `ScreenGrab.cpp`
- `Settings.cpp`
- `Shake.cpp`
- `wintoastlib.cpp`
- `kiero.cpp`

The feature headers contain non-inline globals and are currently safe only
because they are compiled through the single `dllmain.cpp` include chain. If
you add a new `.cpp` that includes those headers, expect duplicate symbols /
ODR violations. Either keep the code header-only and wired into the existing
chain, or do a deliberate header/source split for that module (larger change,
ask first).

## 3. Startup, events, and UI flow

### Startup

```text
DllMain(DLL_PROCESS_ATTACH)
  -> CreateThread(mainThread)
  -> Sleep(5000)
  -> T6SDK::MAIN::PreInit()
  -> register OnGameLoaded listener
  -> OnGameLoaded:
       CustomDvars::Init()
       UIBase::Init()
       TheaterBase::Init()
       optional toast
       Streams::InitStreams()
       T6SDK::MAIN::InitializeDevConsole()
       WeaponAnimations::Init()
       Settings::Settings::Load()
       SetDemoDirectoryDvar()
       Common::LoadAllDemos()
```

`T6SDK::MAIN::DEBUG = true` is set in `Init()`. Do not turn on debug behavior
or extra per-frame logging for a production change.

### Event-driven behavior

T6SDK events are registered with `(uintptr_t)&function`. T6SDK invokes
listeners through untyped function pointers, so a signature mismatch will
usually compile without warning and crash or corrupt state at runtime. Follow
the callback shape used by the owning event. Do not register capturing lambdas
or non-static member functions.

Key events and their owners:

| Event | Owner | Purpose |
|---|---|---|
| `OnGameLoaded` | `dllmain.cpp` | One-time subsystem initialization. |
| `OnEndFrameDrawn` | `UIBase.h` | Main menu, tabs, overlays, notifications, dialogs. |
| `OnActiveFrameDrawn` | `CustomDvars`, `Misc`, `FreeCamera`, `Fog`, `Lights`, `Weapon`, `BoneCamera` | Per-frame updates and dvar polling. |
| `OnTheaterControlsDrawn` | `DollyCamera`, `FreeCamera`, `BoneCamera`, `Lights`, `StreamsMenu` | In-theater control text and markers. |
| `OnKeyPressed` | `UIBase`, `TheaterBase`, `SettingsMenu`, `DemoBrowserMenu` | Global menu keys and keybind capture. |
| `OnGameModeChanged` / `OnCameraModeChanged` | `TheaterBase` / `BoneCamera` | Reset per-mode state. |
| `OnDemoPlaybackInited` | `UIBase` | Initialize sun/fog after demo playback starts. |
| Mouse wheel / clicks | camera and demo browser | Camera roll/FOV, demo navigation. |
| `OnProcessEntity` / `OnCgItemDrawn` | `HoldGun` | Corpse/weapon attachment. |

Registration happens in the relevant `Init()` function. Initialize once; do
not re-register the same listener on every menu open or demo load. The demo
browser currently calls its `Init()` from `Show()`; that is existing debt and
not a pattern to copy.

`DollyCamera` and `Streams` also install raw code hooks
(`h_TickChanged`, `h_DisplayMarkerInfoHook`, and `h_TickIncreasing`) with
inline-assembly callbacks. The streams tick hook is explicitly unhooked on
stop; treat the other installed hooks as process-lifetime state and do not
re-install them on every menu open.

### UI flow

- `UIBase::OnEndFrameDrawn()` is the draw root. It checks `BlankMenuOpened`,
  `DevConsoleOpened`, theater state, and then draws the active tab, dialogs,
  settings, and notifications.
- Tabs are `T6SDK::Drawing::UI_RadioButton` instances declared in
  `UIControls.h`. Each menu draws only while its tab is checked.
- New dialogs are drawn near the end of `OnEndFrameDrawn()` and receive input
  through `UIBase::OnKeyPressed()`; text-box dialogs are guarded by
  `T6SDK::Input::InputLockedByTextBoxDialog`.
- The layout is a 16-column by 40-row grid. Use
  `T6SDK::Drawing::GetGridCellCoords(column, row)` and the existing font-scale
  pattern (`1.0f / 1080.0f * ScreenHeight()`), not hard-coded pixel positions.
- Keep controls as static/inline-static members, not local variables created
  during `Draw()`. T6SDK widgets store callbacks and pointers into their
  backing dvar fields; a destroyed local leaves dangling state.

## 4. Build and dependencies

### Required environment

| Requirement | Notes |
|---|---|
| Windows | The plugin and all libraries are Windows-only. |
| Visual Studio 2022 / MSVC v143 | Project `PlatformToolset` is `v143`; language standard is C++17. |
| Windows 10 SDK | `WindowsTargetPlatformVersion` is `10.0`. |
| Win32 / x86 target | All target libraries are x86. x64 configurations exist but are not supported. |
| External T6SDK checkout | Headers plus an x86 `T6SDK.lib`, built from the T6SDK repository. |
| nlohmann/json single header | Not vendored. The project currently expects `..\..\..\..\Downloads\json.hpp` relative to `t6mvm/`. |

### Build configurations

| Configuration | Platform | Output | Status |
|---|---|---|---|
| `Release` | `Win32` | `t6mvm.Red32n` | Shipping configuration. Links `T6SDK.lib` and `libgmavi.lib`; runs `BuildCounter.ps1`; post-build copies to a hard-coded `D:\Games\Redacted\Plugins`. |
| `ReleaseDebug` | `Win32` | `t6mvm.dll` | Intended dev/debug configuration. The project currently does **not** list `libgmavi.lib` in its dependencies while `Streams.h` calls `gmav_*`; verify link behavior before relying on it. |
| `Debug` | `Win32` | `t6mvm.dll` | Lacks the `libgmavi` include path/library and is not maintained for streams. |
| `Debug`, `Release`, `ReleaseDebug` | `x64` | - | Not supported. The code uses x86 inline assembly, x86 MinHook libraries, and x86 T6SDK/libgmavi imports. Do not claim x64 support. |

Typical command:

```bat
msbuild t6mvm.sln /m /p:Configuration=Release /p:Platform=Win32
```

Or build `t6mvm.sln` from Visual Studio with `Release` / `x86` selected.

Notes:

- The project hard-codes the original author's T6SDK paths
  (`C:\Users\Amyst Amyst\source\repos\T6SDK\...`) and a `D:\Games\Redacted\Plugins`
  post-build copy. These are machine-specific. If you need a local build,
  point the project/property sheet at your own T6SDK and disable the copy; do
  not commit your paths or your output.
- `BuildCounter.ps1` increments `t6mvm/buildcount.txt`, which is ignored by
  git. The UI version string is not wired to that file; do not assume a build
  number update is automatic.
- The checked-in `ReleaseDebug` logs are stale (they reference `AviFile.cpp`
  and an older link line). Do not treat them as the current build state.

### CI

`.github/workflows/build.yml` builds the plugin on `windows-latest` for every
push and pull request, and can be run manually. It:

1. Checks out T6MVM.
2. Checks out T6SDK at the pinned commit
   `d5db363ed79786313159bfdcafe313e06b350b32` into `deps/T6SDK`.
3. Downloads the pinned `nlohmann/json` single header (`v3.11.3`).
4. Patches the two committed `json.hpp` include lines in the disposable CI
   checkout and generates CI-only `Directory.Build.props` files that point
   MSBuild at `deps/T6SDK`/`deps/json`, define `_HAS_STD_BYTE=0` (T6SDK's
   `using namespace std;` otherwise collides with the Windows SDK `byte`
   typedef), and add `windowscodecs.lib`/`psapi.lib`.
5. Builds `T6SDK.lib` (`Release|Win32`) and `t6mvm.vcxproj`
   (`Release|Win32`) into temporary output directories.
6. Uploads `artifacts/t6mvm.Red32n` as a workflow artifact.

The workflow does not modify committed source, does not copy to the game, and
does not test runtime behavior. It has been run successfully on GitHub
Actions; the resulting artifact is a PE32/i386 DLL. If T6SDK is upgraded,
update the pinned SHA in the workflow deliberately and expect source/API
changes. Do not add secrets or game files to the CI build.

### Vendored dependencies

| Dependency | Location | Rule |
|---|---|---|
| MinHook | `MinHook/` | Prebuilt x86 libraries and header. Do not replace or reformat. |
| libgmavi | `libgmavi/` | MIT-licensed AVI writer, prebuilt x86 `.lib` plus C source. Do not reimplement or modify unless the AVI path is the task. |
| ScreenGrab (DirectX Tool Kit) | `t6mvm/ScreenGrab.*` | Microsoft MIT-licensed WIC capture helper. Keep its header intact. |
| kiero | `t6mvm/kiero.*` | D3D11 vtable hook helper. Treat as third-party. |
| wintoastlib | `t6mvm/wintoastlib.*` | Third-party toast library. Treat as third-party. |

Do not add new third-party code or upgrade a vendored dependency without an
explicit request. There is no top-level license; do not add or change licensing
terms.

## 5. Coding rules

### 5.1 Scope and diff discipline

- Read the files you are about to change and their callers before editing.
- One concern per change. Do not mix a bug fix with cleanup, renames, or
  formatting.
- Do not reformat untouched lines. The repository has no enforced formatter and
  existing code is inconsistent; preserving it is better than a large diff.
- Do not create extra documentation files, sample files, or "notes" for a
  change. Update `README.md` or this file when behavior or rules change.
- Do not add speculative abstractions, interfaces, wrappers, or configuration
  that the task does not require.
- Do not add comments that narrate the edit or restate the code. Comments
  should explain non-obvious engine behavior, invariants, or safety checks.
- Do not rename namespaces, files, controls, dvar names, or JSON keys as a
  drive-by improvement. Dvar names and file keys are user-facing contracts.

### 5.2 C++ style

- C++17, MSVC, `/W3`, MultiByte. Do not introduce C++20/23 features.
- Match the indentation of the file you are editing (most feature headers use
  tabs; `dllmain.cpp` uses spaces). Do not convert a file to another style.
- Use `std::` explicitly in new code (`std::string`, `std::vector`, `std::filesystem`).
  Existing code can use unqualified `string`/`vector` because T6SDK's
  `ConsoleLog.h` injects `using namespace std;`. That is a dependency bug, not
  a style to spread. Do not add `using namespace std;`.
- Include `#pragma once` in headers. For project code, include `<StdInclude.h>`
  for the T6SDK surface, then local headers. Do not include `pch.h` from a
  header; `pch.h` is for `.cpp` files.
- Prefer `inline static` for header-level state. Plain non-inline globals in
  headers are only safe while the single-TU architecture above holds. Avoid
  adding new mutable global state; keep state local to the feature namespace.
- Use `std::filesystem` for file/directory operations. Keep Windows separators
  in game-facing path strings.
- Check pointers, indices, and vector sizes before dereferencing. The demo and
  game data paths are not guaranteed to be populated.
- Prefer bounded formatting (`snprintf`, `sprintf_s`) in new code. Do not add
  new unbounded `sprintf` calls. Always pass the destination size where the
  API requires it.
- Errors should be logged with a subsystem tag and, when useful, shown to the
  user with `T6SDK::MAIN::UI_ShowNotification`. Do not silently swallow file,
  JSON, or game-state failures.
- Do not throw exceptions across callbacks invoked by the game or T6SDK.
  Catch locally, log, and return.
- Keep log noise down. Per-frame logging belongs behind a debug flag or should
  be removed; existing noisy logs are not a precedent.

### 5.3 T6SDK and game safety

- Read the T6SDK headers before using a member. If a symbol is not in the
  T6SDK checkout, it does not exist for this project.
- Game addresses are version-specific. Use
  `T6SDK::CrossVersion::CrossValue` with the four variants consistently. If a
  new address or patch is unavoidable, add it to `LocalAddresses.h`, include
  the pattern bytes as a comment, cover all supported variants, and gate it on
  the game version. Never invent a plausible-looking offset.
- Do not store raw T6SDK pointers across game loads or demo reloads without
  re-validating them. `cg`, `DemoPlayback`, and similar pointers are only
  meaningful in the expected mode.
- Check theater/camera mode before touching theater state. Most camera code
  must run only while `T6SDK::Theater::IsInTheater()` and/or a specific camera
  mode is active.
- Preserve global render state. If a feature changes DOF, SSAO, `r_blur`,
  clear colors, timescale, or pause state, restore the previous value when
  disabled. `StreamsCommon` exists for this; use it rather than ad-hoc saves.
- Hooks must be paired. If you install a hook/patch on start, remove or restore
  it on stop/abort. Do not leave a hook installed after a failed start.
- Do not block the render thread or the game's Present path. No sleeps, file
  dialogs, network calls, `std::system`, or large synchronous disk I/O in a
  per-frame callback. AVI close, toast opening, and explorer launching happen
  on stop paths; keep it that way. The stream recorder is the existing
  exception: its per-frame capture is intentionally heavy, so do not add more
  synchronous work there.
- Do not call `exit()` or terminate the process from a game callback.
- Do not use `MessageBox` in normal operation. Log and return; use UI
  notifications for recoverable problems.
- Treat inline assembly/naked functions as load-bearing. Preserve registers,
  flags, and the original call/jump sequence if you touch them.

### 5.4 UI additions

When adding a tab or a control:

1. Add the widget(s) to `UIControls.h` as `inline static`, or keep them local
   to a feature header only if they are never referenced elsewhere.
2. Initialize them in the feature's `Init()`, not in `Draw()`. Use the grid
   helpers and the established font scaling.
3. Register the tab radio button in `UIControls` and in `DrawTabs()`.
4. Call `FeatureMenu::Init()` from `UIBase::Init()` and
   `FeatureMenu::Draw()` from `UIBase::OnEndFrameDrawn()` under the tab check.
5. If the feature has text input, draw its dialog and call `OnInputKey()` in
   `UIBase::OnKeyPressed()`. Do not steal keys from the console or another
   dialog.
6. Add a tooltip for every new control. Follow the existing `^3`/`^5`/`^7`
   color-code style.
7. If the tab writes settings, add `GetCFG()` and wire it into
   `UIBase::CopyCFGtoClipboard()` and/or `MainMenu::SaveAllAsCFG()`.
8. Verify the layout does not overlap the 16x40 grid columns/rows at 16:9
   (and note that other aspect ratios need a visual check).
9. Update `README.md` if the feature is user-visible.

Do not use pixel coordinates for controls. Do not re-create widgets in
`Draw()`; do not use `new` per frame.

### 5.5 Dvars and settings

Dvars are the public control surface. Add them deliberately:

1. Declare `static dvar_s* dvar_name;` in the relevant block of
   `CustomDvars.h`.
2. Register it once in `CustomDvars::Init()` with
   `T6SDK::Dvars::RegisterBool/Float/Int/Vec3/String`.
3. Use a stable `mvm_` prefix (fog keeps existing `z_` names for
   compatibility; do not rename those).
4. Choose sensible defaults and min/max bounds. They are enforced by the dvar
   system and used by UI sliders.
5. If the dvar changes game state, poll it in the owning update function and
   apply only when the value changed or `modified` is set. Clear `modified`
   after applying it.
6. Add a UI control when the dvar is intended for the menu, and add it to the
   feature's `GetCFG()` when it is part of a shareable setup.
7. Document the dvar in `README.md` with type, min, max, default, and a short
   description.
8. Do not remove or rename an existing dvar. Configs, bookmarks, presets, and
   users' muscle memory depend on the names.

`dvar_s` has `current`, `latched`, and `domain` unions plus a `modified` flag.
Do not write directly into unrelated fields. UI sliders should point at the
`current` field; actions that need the game to notice a change should set
`modified = true` or call the appropriate T6SDK setter.

Persistent settings live in `%fs_homepath%\Plugins\t6mvm.json`. They are not
the same thing as dvars. Add a JSON key only for state that must survive a
restart and is not representable as a dvar. Keep loading backward-compatible:
use `data.value("Key", default)` and never assume a key exists.

### 5.6 Files and data formats

Keep existing formats readable and backward-compatible.

| File / key | Format | Notes |
|---|---|---|
| `Plugins/t6mvm.json` | JSON | Settings, keybinds, demos dirs, streams dir, post-stream actions. |
| `Plugins/Sliders/*.json` | JSON | V4-compatible custom slider definitions. Validate all keys and check `FindDvar()` for null before use. |
| Demo `.tags` file | JSON | T6MVM stores `{"Bookmarks": [...]}`; V4 metadata uses `start_tick`, `player_index`, `description`. Preserve both read paths. |
| `.zfog` | JSON | Fog preset with `startDist`, `fadeDist`, colors, sun haze fields. |
| `.t6lights` | Semicolon-separated lines | `TYPE;X;Y;Z;DIRX;DIRY;DIRZ;RADIUS;R;G;B;A;FLICKER;FREQ`; trailing fields are optional. |
| `.t6cr` | CSV rows | `FOV,X,Y,Z,PITCH,YAW,ROLL` per recorded frame; written when `mvm_streams_recordCam` is enabled. |
| `.cfg` | Console commands | Generated `mvm_*` commands; intended to be loaded with T6MVM present. |
| Streams output | Directories/files | `Streams_<date>/<PassName>/<Pass>_<frame>.PNG` or `.JPG`; alternatively `<PassName>.avi`; optionally `<PassName>.mp4` through FFmpeg; optional `Pov_CameraData.t6cr`/`Cine_CameraData.t6cr`. |

Path rules:

- Build runtime paths from `T6SDK::Dvars::GetString(*T6SDK::Dvars::DvarList::fs_homepath)`
  or from the user-selected directory in `Settings`.
- Never hard-code a developer machine path in source.
- Keep the game's backslash path convention inside strings passed to the game
  or Win32 APIs.
- Use `std::filesystem` for checks/creation, but do not assume it will accept
  game-engine path conventions blindly.

### 5.7 FFmpeg output

- FFmpeg is an **external, user-supplied runtime tool**, not a build dependency
  and not vendored. The path is stored in `%fs_homepath%\Plugins\t6mvm.json`
  as `FFmpegPath` and selected from the Streams tab.
- FFmpeg mode captures temporary JPG frames, encodes them to
  H.264 MP4 on a background thread after recording, and deletes the frame
  folders after a successful encode. A failed job keeps its frames and an
  `<output>.mp4.ffmpeg.log` for diagnosis.
- The encoder runs on a worker thread; do not move it back onto the render
  thread. A new recording is blocked while an encode is in flight
  (`IsFFmpegEncoding`).
- The FFmpeg build must contain `libx264`. Do not silently fall back to AVI or
  image sequence when encoding fails; log the error and keep the evidence.
- Do not commit `ffmpeg.exe`, output MP4s, frame folders, or logs. They are
  user data, not repository content.

### 5.8 Security

- Never commit API keys, tokens, user paths, or captured game data. The Alertzy
  key belongs in the user's `t6mvm.json`, never in git.
- Existing stream-completion code invokes `curl` and `explorer` via
  `std::system` with configuration-derived strings. Do not extend that pattern;
  if you touch it, use a properly escaped command line or Win32 process APIs.
- Do not add telemetry, network requests, auto-updaters, or shell execution
  without an explicit request.
- `WinToastHelper` handlers call `exit()` on activation/failure. Do not reuse
  those handlers as a model for new callbacks; a toast click can terminate the
  game.

## 6. Change recipes

### Add a stream pass

1. Derive `IMVMStream` in a new header (or an existing pass header if it is a
   variant of one). Implement `Init()`, `Enable()`, and `Disable()`.
2. Register `mvm_streams_<name>`, `mvm_streams_<name>_ignoreReshade`, and
   `mvm_streams_<name>_preview` dvars in `Init()`.
3. `Enable()` applies the pass-specific render state; `Disable()` restores the
   previous state exactly. Use `StreamsCommon` capture/restore helpers where
   possible.
4. Add a static instance in `Streams.h`, push it in `InitStreams()`, and add a
   UI `UI_StreamPass` slot at the next grid position.
5. Verify image-sequence, AVI, and FFmpeg MP4 paths, including stop and abort
   cleanup.
6. Do not duplicate capture code; the existing paths use WIC (`ScreenGrab`),
   libgmavi, and the FFmpeg post-encoder.

### Add a console command

- Register with `T6SDK::Dvars::Cmd_AddCommandInternal` in the owning module's
  `Init()`, once.
- Use `mvm_` naming.
- Validate `IsInTheater()`, mode, and null pointers before acting.
- Log success/failure; do not silently return if the user invoked a command.
- Do not re-register the command every time a menu opens.

### Add a demo/bookmark field

- `CustomDemoBookmark` is serialized with `NLOHMANN_DEFINE_TYPE_INTRUSIVE`.
  Adding a field changes `.tags`; keep old files readable and do not break the
  V4 metadata branch in `Common::AddBookmarkFromJson`.
- Tick values are milliseconds. Preserve the `mm:ss` display convention:
  `tick / 60000` and `(tick % 60000) / 1000`.
- The metadata write goes through `T6SDK::DemoHandler::WriteTagsMetadata`; do
  not hand-edit demo files outside that path.

## 7. Verification

There are no automated tests. Do not invent a test suite or add a test
framework unless the task explicitly asks for it. Verification means a Windows
build plus a manual smoke test in the game.

### Static checks (safe anywhere)

```bash
# Whitespace and accidental new files
git diff --check
git status --short

# Machine-specific paths or the external json include
grep -RInE 'C:\\Users|D:\\Games|Downloads\\json.hpp' --exclude-dir=.git --exclude=AGENTS.md .

# Dvars registered in CustomDvars.h (compare with README.md manually)
grep -nE 'Register(Bool|Float|Int|Vec3|String)\(' t6mvm/CustomDvars.h

# Encoding / line-ending surprises
file t6mvm/*.h t6mvm/*.cpp
```

Also check that every new `.h`/`.cpp` is listed in `t6mvm/t6mvm.vcxproj` and
`t6mvm/t6mvm.vcxproj.filters` (except deliberately unbuilt legacy files such
as `AviFile.*`).

### Build check

If a Windows build environment is available, either build locally as described
above or use the `Build T6MVM` GitHub Actions workflow. The workflow's
`t6mvm.Red32n` artifact is a compile-checked binary, but it is not a runtime
test.

Resolve warnings introduced by your change. Do not claim a successful build
if the command was not run, and state which configuration was used.

### Manual smoke test (when a test machine is available)

1. Copy the built plugin to the Redacted `Plugins` folder on the test machine.
2. Launch the game and confirm the T6MVM load toast/console messages.
3. Enter Theater, load a demo, and skip back once (README notes tick mismatch
   without this).
4. Open the menu with TAB; visit every tab and confirm controls and tooltips.
5. Exercise the touched feature: camera modes, fog/lights/sun/DOF, demo
   browser, weapon menu, or streams.
6. For streams, record a short capture in image, AVI, and FFmpeg MP4 modes;
   verify output files, temporary-frame cleanup, stop, abort, and that no tick
   hook remains active. For FFmpeg, also verify a bad path/log failure is
   reported and the frames are kept.
7. Restart the game and confirm `t6mvm.json` and settings survive.
8. Close the game cleanly; note any crash/hang even if the feature looked
   correct.

## 8. Known debt (do not copy these patterns)

These are existing rough edges. Do not turn them into conventions, and do not
fix them as unrelated drive-by changes. Fix them only in a dedicated change
with appropriate testing and owner approval.

1. **External, unpinned local dependencies.** T6SDK and `json.hpp` are not
   vendored or pinned by submodule/package manager. CI pins them, but local
   builds are not reproducible across machines. If you update T6SDK, expect
   API drift.
2. **Machine-specific build paths.** `t6mvm.vcxproj` contains
   `C:\Users\Amyst Amyst\source\repos\T6SDK\...` include/library paths and a
   `D:\Games\Redacted\Plugins` post-build copy. `Common.h` and `Settings.h`
   include `..\..\..\..\Downloads\json.hpp`. Do not copy these into new code.
3. **Tracked build junk.** `t6mvm/ReleaseDebug/**` and
   `t6mvm/~AutoRecover.t6mvm.vcxproj*` are committed. `.gitignore` does not
   cover `ReleaseDebug/` or `~AutoRecover*`. Do not add more; delete only as a
   dedicated cleanup.
4. **Dead legacy AVI writer.** `AviFile.cpp`/`.h` are not in the project and
   have no callers. `Streams.h` uses libgmavi instead. Do not resurrect it or
   add it to the project unless you are intentionally replacing the AVI path.
5. **Incomplete local link setup.** The project relies on
   `windowscodecs.lib` (WIC screenshot code) and `psapi.lib` (T6SDK process
   introspection) but does not list them. The CI build injects both. The
   T6SDK `using namespace std;` also requires `_HAS_STD_BYTE=0` with current
   MSVC/Windows SDK headers; CI injects that too. `Release|Win32` links
   `libgmavi.lib`; `ReleaseDebug|Win32` and `Debug|Win32` do not. The checked-in
   tlogs are stale and reference `AviFile.cpp`. Verify linkage before treating
   a configuration as usable.
6. **Unsupported x64 configurations.** They exist in the solution/project but
   the code and vendored libraries are x86. Do not claim x64 support.
7. **README dvar drift.** The table omits several registered dvars, including
   `mvm_demosDirectory`, `mvm_holdgunSearchRadius`, `mvm_ui_grid`, and the
   per-pass `mvm_streams_*` toggles. Keep new entries accurate; do not
   regenerate the whole table blindly.
8. **Hard-coded version display.** `UIBase.h` draws
   `T6MVM v1.0.1 build 1780`; `WinToastHelper.h` uses `1.0.1`; the pre-build
   counter writes an ignored file. Do not bump version numbers unless asked,
   and update all occurrences if you do.
9. **Streams directory settings bug.** `SettingsMenu.h`'s streams-directory
   callback writes `CustomDvars::dvar_demos_directory`; `mvm_streams_directory`
   is commented out in `CustomDvars.h`, yet `Streams::StartStreams` error text
   references it. If you touch streams settings, fix the setter, the dvar
   registration, and the error text together and test.
10. **Duplicate JSON repair helpers.** `Common::repairJson` and
    `Settings::repairJson` are copies. Do not add a third.
11. **Header include cycle.** `Common.h` includes `UIControls.h`, which includes
    `UI_Timeline.h`, which includes `Common.h`. It works due to `#pragma once`
    but is fragile. Do not add more dependencies inside this cycle.
12. **Encoding oddities.** `DemoBrowserMenu.h` has a UTF-8 BOM; `HoldGun.h`
    contains non-UTF-8 bytes; most files are ASCII with LF. Preserve encoding
    and line endings when editing; do not mass-convert files.
13. **No regression tests.** CI only compiles the plugin; it cannot launch the
    game or exercise hooks. Do not pretend a green build is runtime proof.
14. **WinToast process exits.** `WinToastHelper` handlers call `exit()` on
    activation/failure. Do not use them as a template for new callbacks.

## 9. Ask before doing these

Ask the user before:

- Changing the build system, toolset, platform set, dependency layout, or
  obtaining a new third-party library.
- Renaming/removing a dvar, command, settings key, file format, or public
  feature.
- Adding a new game address, patch, detour, or MinHook hook.
- Modifying a vendored dependency or upgrading T6SDK.
- Deleting tracked files or doing repository-wide cleanup/formatting.
- Adding a test framework, changing CI behavior, or adding a new top-level
  directory.
- Making large UI layout changes or a user-visible redesign.
- Bumping the version or preparing a release.

## 10. Definition of done

A change is ready only when all relevant items are true:

- [ ] The diff is limited to the requested behavior.
- [ ] Relevant modules and callers were read first.
- [ ] New files (if any) are added to `.vcxproj` and `.filters`.
- [ ] No build output, logs, local paths, secrets, or IDE files were added.
- [ ] No unrelated renames, reformatting, or "modernization".
- [ ] New dvars are registered, bounded, documented in `README.md`, and
      exposed in the UI/CFG where appropriate.
- [ ] Game state changed by the feature is restored on disable/stop.
- [ ] Error paths log and do not crash or silently succeed.
- [ ] The change builds in the stated configuration (locally or via the CI
      workflow), or the report explicitly says it was not built.
- [ ] Runtime smoke testing status is stated honestly.
- [ ] `README.md` and this file are updated if behavior, commands, or rules
      changed.

Keep this file dense and factual. If a rule changes, update it in the same
change as the code.
