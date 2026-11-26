# Themes

Pick a theme with **Preferences: Color Theme** in the command palette (`Ctrl+Shift+P`) or from the
Settings page. The list shows dark themes first and then light ones. Moving through the list previews
each theme straight away, and `Escape` goes back to the theme you had before.

## Built-in themes

| Theme | Kind | Modelled on | Upstream | License |
|---|---|---|---|---|
| Overheat Dark / Overheat Light | dark / light | Overheat's own | — | Apache-2.0 (this repo) |
| High Contrast (debug) | dark | layout debugging aid | — | Apache-2.0 (this repo) |
| Dark+ | dark | VS Code's classic default dark theme | [microsoft/vscode](https://github.com/microsoft/vscode/tree/main/extensions/theme-defaults) | MIT |
| Light+ | light | VS Code's classic default light theme | [microsoft/vscode](https://github.com/microsoft/vscode/tree/main/extensions/theme-defaults) | MIT |
| One Dark Pro | dark | One Dark Pro | [Binaryify/OneDark-Pro](https://github.com/Binaryify/OneDark-Pro) | MIT |
| Dracula | dark | Dracula | [dracula/visual-studio-code](https://github.com/dracula/visual-studio-code) | MIT |
| GitHub Dark | dark | GitHub Dark Default | [primer/github-vscode-theme](https://github.com/primer/github-vscode-theme) | MIT |
| GitHub Light | light | GitHub Light Default | [primer/github-vscode-theme](https://github.com/primer/github-vscode-theme) | MIT |
| Monokai | dark | Monokai, as bundled with VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/main/extensions/theme-monokai) | MIT |
| Solarized Dark | dark | Solarized (Ethan Schoonover), as bundled with VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/main/extensions/theme-solarized-dark) | MIT |
| Solarized Light | light | Solarized, as bundled with VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/main/extensions/theme-solarized-light) | MIT |
| Nord | dark | Nord | [nordtheme/visual-studio-code](https://github.com/nordtheme/visual-studio-code) | MIT |
| Catppuccin Mocha | dark | Catppuccin | [catppuccin/vscode](https://github.com/catppuccin/vscode), [catppuccin/palette](https://github.com/catppuccin/palette) | MIT |
| Catppuccin Latte | light | Catppuccin | [catppuccin/vscode](https://github.com/catppuccin/vscode), [catppuccin/palette](https://github.com/catppuccin/palette) | MIT |
| Tokyo Night | dark | Tokyo Night | [enkia/tokyo-night-vscode-theme](https://github.com/enkia/tokyo-night-vscode-theme) | MIT |
| Gruvbox Dark | dark | Gruvbox Theme (medium contrast), after morhetz/gruvbox | [jdinhify/vscode-theme-gruvbox](https://github.com/jdinhify/vscode-theme-gruvbox) | MIT |
| Gruvbox Dark Hard | dark | Gruvbox Dark with the `dark0_hard` background | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) | MIT |
| Gruvbox Dark Soft | dark | Gruvbox Dark with the `dark0_soft` background | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) | MIT |
| Gruvbox Light | light | light palette with the "faded" accents, as in `colors/gruvbox.vim` | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) | MIT |
| Gruvbox Light Hard | light | Gruvbox Light with the `light0_hard` background | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) | MIT |
| Gruvbox Light Soft | light | Gruvbox Light with the `light0_soft` background | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) | MIT |
| Ayu Light | light | Ayu | [ayu-theme/vscode-ayu](https://github.com/ayu-theme/vscode-ayu) | MIT |
| Night Owl | dark | Night Owl (Sarah Drasner) | [sdras/night-owl-vscode-theme](https://github.com/sdras/night-owl-vscode-theme) | MIT |

These themes are not affiliated with or endorsed by their original authors. Only colour values were
taken from the upstream themes. The workbench colours come from each theme's `colors`. The syntax
colours come from its TextMate rules for the C++ scopes that match Overheat's token classes, for example
`keyword.control`, `storage.modifier`, `storage.type.built-in`, `entity.name.function`,
`entity.name.type`, `constant.language`, `variable.other.readwrite` and `keyword.control.directive`.
Where an upstream value did not fit Overheat, it was changed slightly:

- **Ayu Light**: the syntax colours keep their hues but are a little darker, so that every token has
  about 3:1 contrast against the background. Upstream, some tokens such as functions are close to 2:1.
- **Catppuccin Latte**: yellow (types, directives) and peach (numbers) are a little darker for the
  same reason.
- **Dracula**: warnings use orange instead of upstream's cyan, which is also the colour of types.
- **Solarized Light**: the selection colour is one step darker than the line highlight. Upstream the
  two colours are the same.
- Overheat has a single tab-strip colour, so inactive tabs use the tab-strip background.

The VS Code default terminal ANSI colours come from
`src/vs/workbench/contrib/terminal/common/terminalColorRegistry.ts`. The Dark+ and Light+ workbench
colours (sidebar, status bar, selection and so on) are VS Code's built-in defaults for those themes.

## Writing your own

Theme files live in `~/.config/overheat/themes/*.toml` and reload when you save them.
`overheat-dark.example.toml` lists every key. A theme can also start from any other theme and
override only a few keys:

```toml
name = "Dracula, red keywords"
base = "Dracula"

[syntax]
keyword = "#ff5555"
```

The built-in themes are written in the same format in `src/theme_builtin.cpp`, so you can copy one
as a starting point.

Keys added after the first version of the format are `editor.bracket_match`, `editor.find_match`,
`editor.find_match_active`, `editor.indent_guide`, `tabs.active_foreground`,
`syntax.keyword_control`, `syntax.constant`, `syntax.type_builtin` and `syntax.variable`. If a file
leaves one of them out but sets its parent, the new key follows the parent. The parents are, in
order: `ui.foreground_muted`, `diagnostic.warning`, `diagnostic.warning`, `gutter.line_number`,
`editor.foreground`, `syntax.keyword`, `syntax.keyword`, `syntax.type` and `editor.foreground`.
Older theme files therefore keep their look. A theme with `dark = false` and no `base` starts from
Overheat Light.
