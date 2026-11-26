# Overheat — TODO

Living list of what is next, known limitations, and ideas. Done items move to
the git log; keep this file short and current.

## Requested (2026-11-26)

- [ ] **Bug:** recursive search in a folder crashes. Find out whether it is the renderer
  (too many result quads/glyphs) or the search itself (memory, unbounded results), then
  add limits. Check how VS Code does it: result caps, streaming results, excluded folders
  and binary/large-file skipping
- [ ] Workspaces like VS Code's, but smarter and never auto-opened on startup
- [x] More bundled fonts: Fira Code, Cascadia Code, Iosevka Term, Maple Mono, Source Code Pro, Hack, IBM Plex Mono, Geist Mono, Monaspace Neon (assets/fonts/README.md)
- [ ] More themes, using popular VS Code themes as references
- [x] Window transparency on Linux and Windows (`opacity`; per-pixel where the Vulkan surface composites alpha, else whole-window opacity on X11/Windows)
- [ ] (Optional, low priority) Wobbly windows or other KDE-inspired animations
- [ ] Full-featured status bar
- [ ] File-type icons in the explorer and tabs, like VS Code's but in our own style
- [ ] **New logo/icon:** a flat but 3D-looking flame in the style of Obsidian's logo,
  centred, with no background. Keep the current colours, using contrast between them for
  shading. No drop light

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
- Remote search briefly blocks the UI while `grep` runs on the host
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
