# Overheat — TODO

Living list of what is next, known limitations, and ideas. Done items move to
the git log; keep this file short and current.

## Requested (2026-11-26)

- [ ] Full-featured status bar (keep reading `status.background`; Dark+/Light+ use a blue bar)
- [ ] File-type icons in the explorer and tabs, like VS Code's but in our own style
- [ ] **New logo/icon:** a flat but 3D-looking flame in the style of Obsidian's logo,
  centred, with no background. Keep the current colours, using contrast between them for
  shading. No drop light
- [ ] **Command Center** like VS Code's: a search-box strip at the top centre showing the
  open file / workspace name (e.g. `app.cpp — Overheat`). Clicking it opens Quick Open
  (Ctrl+P); Ctrl+Shift+P opens the same box in `>` command mode, and recently opened files
  are listed first
- [ ] **Verify on real Windows: text no longer scales while resizing the window.** Cause:
  during a drag-resize Windows runs a modal loop, no frames were drawn, and DWM stretched
  the stale frame. Fixed in b4b5532 by redrawing from SDL's live-resize EXPOSED event
  watch; untested on Windows hardware
- [ ] Layout at small sizes (seen at 520x380): the sidebar and terminal keep their size and
  the editor collapses to nothing. Give the editor a minimum size and shrink or auto-hide
  the sidebar and terminal first, like VS Code
- [ ] (Optional, low priority) Wobbly windows or other KDE-inspired animations

## Follow-ups from the 2026-11-26 batch

- [ ] Search: multi-threaded local search (no-match scan of 41k files takes ~2 s)
- [ ] Workspaces: test Remote-SSH workspaces against a real host; mixed local + remote
  folders; apply edits to a workspace's `[settings]` live; git for non-primary roots
- [ ] Transparency: opaque popups / palette option; background blur (KDE, Windows mica)
- [ ] Transparency on Windows: test on real drivers. Per-pixel needs PRE/POST_MULTIPLIED
  `compositeAlpha` (rare on Windows); reliable per-pixel needs presenting via
  DirectComposition (Vulkan → D3D11 interop)
- [ ] Fonts: bold / italic faces; consider dropping Iosevka (8 MB of the 10.6 MB)
- [ ] Themes: Tokyo Night Day; string-escape and namespace colours
- [ ] Ctrl+K Ctrl+T (theme picker chord) doesn't fire in `--script` mode

## From the editor survey (2026-11-26)

- [ ] **Clickable `file:line:col`** in the terminal and build output (Acme/Geany): Ctrl+click
  opens the file at that position. Also a one-key **build/run task** (configurable command,
  e.g. F5/Ctrl+B) whose compiler errors land in the Problems panel
- [ ] **Multibuffers** (Zed): search results, references and diagnostics shown as one
  *editable* document of excerpts, with edits written back to the source files. One
  mechanism for the search results, find references and the Problems panel
- [ ] **Syntax-aware selection** (Helix/Kakoune): expand/shrink the selection to the
  enclosing tree-sitter node, and select the next/previous sibling node with multiple
  cursors. The selection model only, not modal editing
- [ ] **True portable mode** (SciTE/Notepad++): when a marker file (e.g. `portable.txt`)
  sits next to the exe, keep settings, themes and workspaces next to it instead of
  `%APPDATA%` / `~/.config`; ship the Windows portable zip with the marker

## Next up

- [ ] Hover tooltips (LSP `textDocument/hover`) with a small markdown renderer
- [ ] Rename symbol (F2), format document / selection, organize imports
- [ ] Document symbols (Ctrl+Shift+O) and workspace symbols (Ctrl+T)
- [ ] Find all references with a peek view
- [ ] Problems panel (diagnostics for all open files, click to jump)
- [ ] Remote-SSH: git status/blame/commit over the SSH connection
- [ ] Remote-SSH: run language servers on the remote host
- [ ] Remote search without blocking the UI (dedicated SSH channel / thread)
- [ ] AI chat: markdown rendering, code-block copy/apply, saved conversations
- [ ] AI chat: tool use (read files, apply edits with a diff preview)

## Editor

- [ ] Cursor up/down by visual row when word wrap is on
- [ ] Word wrap for files over 2 MB (incremental wrap map)
- [ ] Configurable tab width and "insert spaces" (currently 4 / spaces)
- [ ] Bold / italic font faces for syntax styles
- [ ] Column (box) selection with Alt+Shift+drag
- [ ] Sticky scroll (current scope pinned at the top)
- [ ] Code folding
- [ ] More tree-sitter grammars (Python, Rust, JS/TS, Go, JSON, Markdown, ...)
- [ ] Diff editor (side-by-side / inline) for git changes

## Workbench

- [ ] More than two editor groups; vertical splits
- [ ] Multiple terminals (tabs) and split terminals
- [ ] Hover fades and press states for buttons (animation polish)
- [ ] Settings UI: search box, tab width, terminal font, per-language settings
- [ ] Command palette: recently used commands first
- [ ] Explorer: create / rename / delete files and folders, drag & drop
- [ ] Notifications area for long-running tasks (git, SSH, LSP start-up)

## Known limitations

- Ligatures are on by default; JetBrains Mono turns `/=` into `≠` (toggle in Settings)
- The terminal does not reflow old output when resized
- Split editors support two groups only
- Remote search briefly blocks the UI while `grep` runs on the host (needs a thread-safe
  SSH channel; local search already streams on a worker thread)
- The OMP chat backend shows the CLI's "Working..." line in replies

## Platform & release

Release builds: `packaging/windows/build.sh` (installer + portable zip),
`packaging/linux/build.sh` (tarball, .deb, AppImage), `packaging/arch/build.sh`.

- Portable Linux packages need glibc >= 2.43 when built on Arch; `docker/build.sh linux`
  builds them on Ubuntu 24.04 (glibc 2.39)
- [ ] Build release Linux assets with `docker/build.sh linux` (glibc 2.38 instead of 2.43)
- [ ] Test on real Windows 10/11 hardware (verified under Wine only): HiDPI, IME, ConPTY resize
- [ ] Windows: settings UI for the terminal shell (now `terminal_shell` in settings.toml)
- [ ] Windows: Remote-SSH agent auth via Pageant / OpenSSH agent pipe
- [ ] Code signing for the Windows installer
- [ ] macOS port (Metal via MoltenVK, PTY, packaging)
- [ ] Auto-update channel
