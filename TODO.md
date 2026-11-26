# Overheat — TODO

Living list of what is next, known limitations, and ideas. Done items move to
the git log; keep this file short and current.

## C, C++ and Rust IDE core (next round)

Goal: a complete edit → build → fix → debug loop for C, C++ and Rust, on the local machine
first, then over Remote-SSH. Language servers: clangd (C/C++) and rust-analyzer (Rust).
Builds: CMake, Make and Cargo. Debugging: gdb's built-in DAP (`gdb -i dap`, gdb >= 14)
for C/C++ and Rust, plus lldb-dap / CodeLLDB when installed.

- [ ] **Build tasks:** auto-detect CMake (configure, build, presets), Make and Cargo
  (build, run, test, check, clippy), plus a project task file and an import of
  `.vscode/tasks.json`. Ctrl+Shift+B builds, F5 runs, and Run Task… lists the rest.
  Output goes to a task terminal; Ctrl+Break cancels.
- [ ] **Problem matchers and Problems panel:** parse gcc/clang (`file:line:col: error:`),
  MSVC (`file(line,col): error`), CMake and rustc/cargo output (`--> file:line:col`, or
  `--message-format=json`), including notes and "in expansion of" chains. A Problems panel
  lists LSP diagnostics and build errors together, with severity filters; F8 /
  Shift+F8 step through them, and the jump line highlights the location.
- [ ] **Clickable `file:line:col`** in the terminal and task output (Ctrl+click), with
  paths resolved against the task's cwd.
- [ ] **Linting:** clang-tidy through clangd (`.clang-tidy`), `cargo clippy` as a task
  with its own matcher, quick fixes / code actions (Ctrl+.) from both, and format on save
  with clang-format / rustfmt (via the LSP).
- [ ] **LSP navigation:** hover tooltips with a small markdown renderer; Ctrl+click and
  hover-underline go-to-definition; Peek Definition (Alt+F12); Go to Declaration / Type
  Definition / Implementation; Find All References with a peek view; rename (F2) with a
  preview; document symbols (Ctrl+Shift+O) and workspace symbols (Ctrl+T); clangd's
  switch header/source (Alt+O); inlay hints (parameter names, types) for rust-analyzer
  and clangd; back/forward navigation history (Ctrl+Alt+- / Ctrl+Shift+-).
- [ ] **Debugger (DAP):**
  - launch and attach configurations: the CMake target or Cargo binary, args, env,
    cwd; `.vscode/launch.json` import
  - breakpoints (gutter click, F9), conditional breakpoints and logpoints
  - stepping: continue (F5), step over/into/out (F10/F11/Shift+F11), restart, stop
  - panels for call stack, threads, variables / watch with expandable structs and
    pretty printers (libstdc++ and Rust's gdb printers), and a debug console
  - inline values next to the code, and the current line highlighted
  - Rust: `rust-gdb` printers; Rust panics break automatically
- [ ] **Multibuffers** (Zed): references, search results and the Problems panel as one
  editable list of excerpts (see "From the editor survey").
- [ ] **Remote:** run clangd / rust-analyzer, builds and gdb on the Remote-SSH host
  (stdio over an SSH channel), with path mapping between remote and local paths.
- [ ] Tests: a small C, C++ (CMake) and Rust (Cargo) sample project in `tests/projects/`
  driving all of the above in `--script` mode. On this machine clangd, clang-tidy, gdb 18,
  cargo and rustc are installed; rust-analyzer and lldb-dap are not (install rust-analyzer
  to test Rust LSP).

## Requested (2026-11-26)

- [ ] **New logo/icon:** a flat but 3D-looking flame in the style of Obsidian's logo,
  centred, with no background. Keep the current colours, using contrast between them for
  shading. No drop light
- [ ] **Verify on real Windows: text no longer scales while resizing the window.** Cause:
  during a drag-resize Windows runs a modal loop, no frames were drawn, and DWM stretched
  the stale frame. Fixed in b4b5532 by redrawing from SDL's live-resize EXPOSED event
  watch; untested on Windows hardware
- [ ] **4coder-style indentation**: indentation computed from the code structure
  (tree-sitter scopes, brackets, continued statements) instead of typed. Every edit
  re-indents the affected lines automatically. Optionally "virtual whitespace": leading
  whitespace is drawn from the computed layout and the file's own indentation is ignored,
  with a setting to write the computed indentation back on save. Per-language rules;
  off by default, with an explicit "Reindent selection / file" command either way
- [ ] **4coder-style highlighting of brackets and scopes**: colour parentheses, brackets and
  braces by nesting depth; tint the backgrounds of enclosing scopes around the cursor (one
  shade per level); highlight the matching pair and the scope boundaries more strongly
  than the current bracket match
- [ ] **4coder-style underlines**: underline the token under the cursor and its other
  occurrences, and the lines of the current jump/compile-error location. Make the style
  (underline, box, background) configurable per theme
- [ ] **Hex editor:** "Reopen With → Hex Editor" for any file, and the default for
  detected binaries. Offset / hex / ASCII columns, goto offset, search for bytes or text,
  in-place byte editing with undo and save, and selection kept in sync between the hex
  and ASCII columns. Large files are paged or memory-mapped, not loaded whole; it works
  over SFTP too
- [ ] **Toggle the status bar:** View → Toggle Status Bar (palette + settings.toml
  `status_bar = false`), like VS Code; the monitor strip and layout adapt
- [ ] **VS Code-like split view** (today: two side-by-side groups only):
  - a grid of editor groups: split right/left/up/down in any nesting (Ctrl+\, the
    palette, the tab context menu), with draggable sashes and double-click to even out
  - drag a tab to an edge or the centre of a group, with drop-zone overlays, to split
    or move it; drag between groups
  - Ctrl+1/2/3… focus a group, Ctrl+K arrows move focus, and move or copy the editor
    to the next group
  - the same file open in two groups shares one buffer (edits, undo, dirty state),
    with independent cursors and scroll
  - maximize or restore a group, close a group and merge its tabs, and "Join All Groups"
  - layouts saved in sessions and workspaces
  - split terminals in the panel, and later a side-by-side diff view on the same layout
- [ ] **Folder picker navigation** (File: Open Folder…, Add Folder to Workspace, remote
  folders). Today it is a typed path. Make it a browsable list like VS Code's simple file
  dialog (`files.simpleDialog`, also used for Remote-SSH); check its exact behaviour
  first:
  - the list shows the subfolders (and files, greyed) of the current path, with a `..` entry
  - **Enter** on a highlighted folder goes into it (one level forward); Enter on the
    current path, or an "OK" item, opens it
  - **Backspace** on an empty filter, or right after a `/`, goes up one folder
  - typing filters the list, Tab completes the path, and `~` and absolute paths work
  - the same picker serves local and SFTP paths
- [ ] **Multiple terminal instances:**
  - several terminals in the panel as tabs or a side list, each with its own shell,
    cwd and title (from the shell / OSC 0/2), plus new, kill and rename actions
  - split terminals side by side within the panel
  - a shell profile picker (bash, zsh, fish, pwsh, cmd, ssh host) and a default profile
    setting
  - Ctrl+Shift+` for a new terminal, and keys to focus the next/previous terminal
  - local and remote terminals can coexist; terminals restore in sessions and
    workspaces (cwd and profile, not the process)
  - task runs (see build tasks) get their own terminal
- [ ] **Build, lint and navigation like VS Code**
  - **Build tasks:** a project task file (like `.vscode/tasks.json`: name, command, cwd,
    env, default build task) with Run Build Task (Ctrl+Shift+B) and Run Task… in the
    palette. Output goes to a task terminal, and problem matchers (gcc/clang, MSVC, rustc,
    tsc, generic `file:line:col`) feed the Problems panel. Auto-detect CMake, Make, Cargo
    and npm scripts. Also import `.vscode/tasks.json` when present. Works over Remote-SSH.
    Builds on the clickable-errors item below.
  - **Linting:** today only the language server's diagnostics show (clangd). Add
    clang-tidy through clangd (`.clang-tidy`), external linters per language (ruff,
    eslint, shellcheck…) run on save with problem matchers, quick fixes / code actions
    (Ctrl+.), and a Problems panel listing all diagnostics with severity filters.
  - **Go to definition:** F12 exists. Add Ctrl+click and hover underline on symbols,
    Peek Definition (Alt+F12) inline, Go to Declaration / Type Definition /
    Implementation, a picker when there are several results, and back/forward
    navigation history. Alt+Left/Right in the `vscode` keymap preset; the default preset
    uses Alt+Left/Right for subword motion, so bind VS Code's Linux keys there
    (Ctrl+Alt+- back, Ctrl+Shift+- forward). It must work for remote files too.

## Follow-ups from the 2026-11-26 batch

- [ ] Search: multi-threaded local search (no-match scan of 41k files takes ~2 s)
- [ ] Workspaces: test Remote-SSH workspaces against a real host; mixed local + remote
  folders; apply edits to a workspace's `[settings]` live; git for non-primary roots
- [ ] Transparency: background blur (KDE, Windows mica)
- [ ] Transparency on Windows: test on real drivers. Per-pixel needs PRE/POST_MULTIPLIED
  `compositeAlpha` (rare on Windows); reliable per-pixel needs presenting via
  DirectComposition (Vulkan → D3D11 interop)
- [ ] Fonts: bold / italic faces; consider dropping Iosevka (8 MB of the 10.6 MB)
- [ ] Themes: Tokyo Night Day; string-escape and namespace colours

## From the editor survey (2026-11-26)

- [ ] **Clickable `file:line:col`** in the terminal and build output (Acme/Geany): Ctrl+click
  opens the file at that position. Also a one-key **build/run task** (configurable command,
  e.g. F5/Ctrl+B) whose compiler errors land in the Problems panel
- [ ] **Multibuffers** (Zed): search results, references and diagnostics shown as one
  *editable* document of excerpts, with edits written back to the source files. One
  mechanism for the search results, find references and the Problems panel
- [ ] **True portable mode** (SciTE/Notepad++): when a marker file (e.g. `portable.txt`)
  sits next to the exe, keep settings, themes and workspaces next to it instead of
  `%APPDATA%` / `~/.config`; ship the Windows portable zip with the marker

## Next up

- [ ] Remote-SSH: git status/blame/commit over the SSH connection
- [ ] Remote-SSH: run language servers on the remote host
- [ ] Remote search without blocking the UI (dedicated SSH channel / thread)
- [ ] AI chat: markdown rendering, code-block copy/apply, saved conversations
- [ ] AI chat: tool use (read files, apply edits with a diff preview)

## Editor

- [ ] Cursor up/down by visual row when word wrap is on
- [ ] 4coder-style deletes: Ctrl+Backspace/Delete by alphanumeric boundary, Alt+Backspace/Delete
  "snipe" (whitespace or token boundary) in the `overheat` keymap preset
- [ ] Syntax selection: an "inside the brackets" step between a node and its parent (VS Code)
- [ ] Word wrap for files over 2 MB (incremental wrap map)
- [ ] Large files: read-only hex view for binaries; mmap / lazy line indexing instead of
  reading the whole file (today: card above 50 MB, refused above 256 MB)
- [ ] Remote-SSH: test the binary / large-file checks against a real SFTP server
- [ ] Configurable tab width and "insert spaces" (currently 4 / spaces)
- [ ] Bold / italic font faces for syntax styles
- [ ] Column (box) selection with Alt+Shift+drag
- [ ] Sticky scroll (current scope pinned at the top)
- [ ] Code folding
- [ ] More tree-sitter grammars (Python, Rust, JS/TS, Go, JSON, Markdown, ...)
- [ ] Diff editor (side-by-side / inline) for git changes

## Workbench

- [ ] Hover fades and press states for buttons (animation polish)
- [ ] Settings UI: search box, tab width, terminal font, per-language settings
- [ ] Command palette: recently used commands first
- [ ] Explorer: create / rename / delete files and folders, drag & drop
- [ ] Notifications area for long-running tasks (git, SSH, LSP start-up)

## Known limitations

- Ligatures are on by default; JetBrains Mono turns `/=` into `≠` (toggle in Settings)
- The terminal does not reflow old output when resized
- Lines over 64 KiB (minified files) are drawn only up to 64 KiB ("long line truncated");
  the cursor and edits still reach the whole line
- Split editors support two side-by-side groups only (see "VS Code-like split view")
- Remote search briefly blocks the UI while `grep` runs on the host (needs a thread-safe
  SSH channel; local search already streams on a worker thread)
- The OMP chat backend shows the CLI's "Working..." line in replies

## Platform & release

Release builds: `packaging/windows/build.sh` (installer + portable zip),
`packaging/linux/build.sh` (tarball, .deb, AppImage), `packaging/arch/build.sh`.

- Release Linux packages are built with `docker/build.sh linux` (Ubuntu 24.04, glibc >= 2.38);
  host builds on Arch need glibc >= 2.43
- [ ] Test on real Windows 10/11 hardware (verified under Wine only): HiDPI, IME, ConPTY resize
- [ ] Windows: settings UI for the terminal shell (now `terminal_shell` in settings.toml)
- [ ] Windows: Remote-SSH agent auth via Pageant / OpenSSH agent pipe
- [ ] Code signing for the Windows installer
- [ ] macOS port (Metal via MoltenVK, PTY, packaging)
- [ ] Auto-update channel
