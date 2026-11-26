# Bundled fonts

Everything in this folder is installed next to Overheat (`fonts/` on Windows,
`share/overheat/fonts` on Linux, copied to `<build>/fonts` for build-tree runs)
and shows up in *Preferences: Select Font Family* marked "bundled". Only the
Regular face of each family is shipped: the renderer draws with one face.

Every font is redistributed unmodified under its own license, which sits next
to it. Fonts were taken from the official GitHub release of each project, at
the pinned version below.

| Family | File | License | Version | Size |
|---|---|---|---|---|
| JetBrainsMono Nerd Font Mono (default) | `JetBrainsMonoNerdFontMono-Regular.ttf` | SIL OFL 1.1, `JetBrainsMono-OFL.txt` | JetBrains Mono 2.304, Nerd Fonts 3.5.1 | 2.4 MB |
| Fira Code | `FiraCode-Regular.ttf` | SIL OFL 1.1, `FiraCode-OFL.txt` | 6.2 | 283 KB |
| Cascadia Code | `CascadiaCode-Regular.ttf` | SIL OFL 1.1, `CascadiaCode-OFL.txt` | 2407.24 | 584 KB |
| Iosevka Term | `IosevkaTerm-Regular.ttf` | SIL OFL 1.1, `Iosevka-OFL.txt` | 34.9.1 (unhinted) | 8.0 MB |
| Maple Mono | `MapleMono-Regular.ttf` | SIL OFL 1.1, `MapleMono-OFL.txt` | 7.9 | 259 KB |
| Source Code Pro | `SourceCodePro-Regular.ttf` | SIL OFL 1.1, `SourceCodePro-OFL.txt` | 2.042R-u | 205 KB |
| Hack | `Hack-Regular.ttf` | MIT + Bitstream Vera license, `Hack-LICENSE.txt` | 3.003 | 302 KB |
| IBM Plex Mono | `IBMPlexMono-Regular.ttf` | SIL OFL 1.1, `IBMPlexMono-OFL.txt` | 2.5.0 | 169 KB |
| Geist Mono | `GeistMono-Regular.ttf` | SIL OFL 1.1, `GeistMono-OFL.txt` | 1.9.2 | 145 KB |
| Monaspace Neon | `MonaspaceNeon-Regular.otf` | SIL OFL 1.1, `Monaspace-OFL.txt` | 1.400 | 346 KB |

Fira Code, Cascadia Code, Maple Mono and Monaspace have programming ligatures
(or Monaspace's "texture healing"); *Preferences: Toggle Font Ligatures* turns
the `calt`/`liga` features off for them.

## Sources

Release archive (SHA-256 of the archive) and path of the file inside it:

- **Fira Code 6.2**:
  https://github.com/tonsky/FiraCode/releases/download/6.2/Fira_Code_v6.2.zip
  (`0949915ba8eb24d89fd93d10a7ff623f42830d7c5ffc3ecbf960e4ecad3e3e79`), `ttf/FiraCode-Regular.ttf`.
  License: https://raw.githubusercontent.com/tonsky/FiraCode/6.2/LICENSE
- **Cascadia Code 2407.24**:
  https://github.com/microsoft/cascadia-code/releases/download/v2407.24/CascadiaCode-2407.24.zip
  (`e67a68ee3386db63f48b9054bd196ea752bc6a4ebb4df35adce6733da50c8474`), `ttf/static/CascadiaCode-Regular.ttf`.
  License: https://raw.githubusercontent.com/microsoft/cascadia-code/v2407.24/LICENSE
- **Iosevka Term 34.9.1**:
  https://github.com/be5invis/Iosevka/releases/download/v34.9.1/PkgTTF-Unhinted-IosevkaTerm-34.9.1.zip
  (`ece371d1073015b3bdc4ccef916c9ffcd3e824487520dd2f7b2e4fdf4b638daf`), `IosevkaTerm-Regular.ttf`.
  License: https://raw.githubusercontent.com/be5invis/Iosevka/v34.9.1/LICENSE.md
- **Maple Mono 7.9**:
  https://github.com/subframe7536/maple-font/releases/download/v7.9/MapleMono-TTF.zip
  (`3a35f8f0669bef3dded9df208cc4526a6f7573210e134816e9084a8981271d75`), `MapleMono-Regular.ttf`, `LICENSE.txt`.
- **Source Code Pro 2.042R-u**:
  https://github.com/adobe-fonts/source-code-pro/releases/download/2.042R-u%2F1.062R-i%2F1.026R-vf/TTF-source-code-pro-2.042R-u_1.062R-i.zip
  (`0c85bac90d15c040b82939aa92bc8404420fccc02e37bbcb9c93a7f21abb52c6`), `TTF/SourceCodePro-Regular.ttf`.
  License: https://raw.githubusercontent.com/adobe-fonts/source-code-pro/d3f1a5962cde503f9409c21e58527611d4a19ef1/LICENSE.md
- **Hack 3.003**:
  https://github.com/source-foundry/Hack/releases/download/v3.003/Hack-v3.003-ttf.zip
  (`0c2604631b1f055041c68a0e09ae4801acab6c5072ba2db6a822f53c3f8290ac`), `ttf/Hack-Regular.ttf`.
  License: https://raw.githubusercontent.com/source-foundry/Hack/v3.003/LICENSE.md
- **IBM Plex Mono 2.5.0**:
  https://github.com/IBM/plex/releases/download/%40ibm%2Fplex-mono%402.5.0/ibm-plex-mono.zip
  (`6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c`),
  `ibm-plex-mono/fonts/complete/ttf/IBMPlexMono-Regular.ttf`, `ibm-plex-mono/LICENSE.txt`.
- **Geist Mono 1.9.2**:
  https://github.com/vercel/geist-font/releases/download/v1.9.2/geist-font-v1.9.2.zip
  (`15f066d58dcb99fb10014a49913009fdaf41e464cfb8851a44b44e593c95d3d8`),
  `geist-font/GeistMono/ttf/GeistMono-Regular.ttf`, `geist-font/OFL.txt`.
- **Monaspace Neon 1.400**:
  https://github.com/githubnext/monaspace/releases/download/v1.400/monaspace-static-v1.400.zip
  (`ab66d71be751495f679727332a3345597943bd4d7beebca03f5cde04bf994de7`),
  `Static Fonts/Monaspace Neon/MonaspaceNeon-Regular.otf`.
  License: https://raw.githubusercontent.com/githubnext/monaspace/v1.400/LICENSE
- **JetBrainsMono Nerd Font Mono**: the `JetBrainsMonoNerdFontMono-Regular.ttf`
  face of the Nerd Fonts 3.5.1 JetBrainsMono patch (https://github.com/ryanoasis/nerd-fonts).

SHA-256 of the shipped files:

```
5992ab9640e2df491b2f609467b1de60e8bc39b2c28db184342a0592d98f6117  FiraCode-Regular.ttf
c33ef522cdfeff99907fb54f3e97152bb18bfb9b56ec2fef4d4ceec51c8974a4  CascadiaCode-Regular.ttf
73d709d1536fa4e356212751bb6778f8f7d400fe7837bfd690c3167295faaa45  IosevkaTerm-Regular.ttf
369583f0b555f33bdb6fdbcc2a6840a7a61a1d72c008a57a665baa36d7d5adf6  MapleMono-Regular.ttf
74bd80d3e42a08517cd7e1108ba3d86f2da29ac0f3065be95e0357956ab9db37  SourceCodePro-Regular.ttf
15f55cc0c85a2988d2b4b3a8cdb5d77fdfbaf319e1bb5309d725db9818fb7125  Hack-Regular.ttf
7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43  IBMPlexMono-Regular.ttf
686914333e77a620c85b35481064788deb33fc25f69901bdbf296c1bbc26a7fe  GeistMono-Regular.ttf
0daf2cd6cbd780b1a9426cb25c17e80f22844051f988a5133ad54b2751b0b8af  MonaspaceNeon-Regular.otf
f2a5ea6cfab397445ffab00c0370927b66d61e560a05db5db271b42006381c1a  JetBrainsMonoNerdFontMono-Regular.ttf
```

Popular proprietary coding fonts (Operator Mono, MonoLisa, Berkeley Mono,
PragmataPro, Dank Mono, SF Mono, Consolas) are not redistributable and are not
bundled; when installed on the system they appear in the picker like any other
monospace font.
