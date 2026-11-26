// File name -> icon table. To add a file type, add a row below: an exact name, an
// extension (without the dot; compound ones like "d.ts" work) or a name prefix. To add a
// new mark, draw it in assets/icons/build_icons.py and rebuild the font (see its README).
#include "ui/file_icons.h"
#include "ui/file_icon_glyphs.h"
#include <string>
#include <unordered_map>

namespace oh::file_icons {

namespace {

namespace g = icon_glyph;

struct Rule {
    const char* key;
    uint32_t mark;
    Hue hue;
};

// Exact file names, lowercase.
const Rule kNames[] = {
    {"cmakelists.txt", g::cmake, Hue::Green},
    {"makefile", g::build, Hue::Orange},
    {"gnumakefile", g::build, Hue::Orange},
    {"meson.build", g::build, Hue::Green},
    {"build.ninja", g::build, Hue::Gray},
    {"dockerfile", g::docker, Hue::Cyan},
    {"containerfile", g::docker, Hue::Cyan},
    {".dockerignore", g::docker, Hue::Gray},
    {"docker-compose.yml", g::docker, Hue::Cyan},
    {"docker-compose.yaml", g::docker, Hue::Cyan},
    {"compose.yml", g::docker, Hue::Cyan},
    {"compose.yaml", g::docker, Hue::Cyan},
    {".gitignore", g::git, Hue::Red},
    {".gitattributes", g::git, Hue::Red},
    {".gitmodules", g::git, Hue::Red},
    {".gitkeep", g::git, Hue::Gray},
    {".mailmap", g::git, Hue::Gray},
    {"cargo.toml", g::rust, Hue::Orange},
    {"cargo.lock", g::lock, Hue::Orange},
    {"pyproject.toml", g::python, Hue::Cyan},
    {"requirements.txt", g::python, Hue::Cyan},
    {"go.mod", g::go, Hue::Cyan},
    {"go.sum", g::lock, Hue::Cyan},
    {"package.json", g::json, Hue::Green},
    {"package-lock.json", g::lock, Hue::Gray},
    {"tsconfig.json", g::ts, Hue::Blue},
    {"jsconfig.json", g::js, Hue::Yellow},
    {".clang-format", g::config, Hue::Gray},
    {".clang-tidy", g::config, Hue::Gray},
    {".clangd", g::config, Hue::Gray},
    {".editorconfig", g::config, Hue::Gray},
    {".env", g::key, Hue::Yellow},
    {".npmrc", g::config, Hue::Red},
    {"pkgbuild", g::package, Hue::Blue},
    {"vagrantfile", g::config, Hue::Blue},
    {"gemfile", g::ruby, Hue::Red},
    {"rakefile", g::ruby, Hue::Red},
    {"flake.nix", g::nix, Hue::Cyan},
    {"todo.md", g::markdown, Hue::Yellow},
};

// Name prefixes, lowercase: "readme.md", "license-mit", "changelog.txt"...
const Rule kPrefixes[] = {
    {"readme", g::readme, Hue::Blue},
    {"license", g::license, Hue::Yellow},
    {"licence", g::license, Hue::Yellow},
    {"copying", g::license, Hue::Yellow},
    {"unlicense", g::license, Hue::Yellow},
    {"changelog", g::clock, Hue::Green},
    {"changes", g::clock, Hue::Green},
    {"history", g::clock, Hue::Green},
    {"dockerfile", g::docker, Hue::Cyan},
    {"makefile", g::build, Hue::Orange},
    {".env.", g::key, Hue::Yellow},
};

// Extensions, lowercase, without the dot.
const Rule kExtensions[] = {
    // C family
    {"c", g::c, Hue::Blue},
    {"h", g::h, Hue::Purple},
    {"cpp", g::cpp, Hue::Indigo}, {"cc", g::cpp, Hue::Indigo}, {"cxx", g::cpp, Hue::Indigo},
    {"c++", g::cpp, Hue::Indigo}, {"ixx", g::cpp, Hue::Indigo}, {"cppm", g::cpp, Hue::Indigo},
    {"hpp", g::hpp, Hue::Purple}, {"hh", g::hpp, Hue::Purple}, {"hxx", g::hpp, Hue::Purple},
    {"h++", g::hpp, Hue::Purple}, {"inl", g::hpp, Hue::Purple}, {"ipp", g::hpp, Hue::Purple},
    {"cs", g::csharp, Hue::Green}, {"csx", g::csharp, Hue::Green},
    {"m", g::c, Hue::Cyan}, {"mm", g::cpp, Hue::Cyan},
    // Systems
    {"rs", g::rust, Hue::Orange},
    {"go", g::go, Hue::Cyan},
    {"zig", g::zig, Hue::Orange}, {"zon", g::zig, Hue::Orange},
    {"java", g::java, Hue::Red}, {"jar", g::java, Hue::Red}, {"class", g::java, Hue::Gray},
    {"kt", g::kotlin, Hue::Purple}, {"kts", g::kotlin, Hue::Purple},
    {"hs", g::haskell, Hue::Purple}, {"lhs", g::haskell, Hue::Purple},
    {"nix", g::nix, Hue::Cyan},
    // Scripting
    {"py", g::python, Hue::Cyan}, {"pyi", g::python, Hue::Cyan}, {"pyw", g::python, Hue::Cyan},
    {"ipynb", g::python, Hue::Orange},
    {"lua", g::lua, Hue::Indigo},
    {"rb", g::ruby, Hue::Red}, {"gemspec", g::ruby, Hue::Red},
    {"php", g::php, Hue::Indigo},
    {"vim", g::vim, Hue::Green}, {"vimrc", g::vim, Hue::Green},
    {"sh", g::shell, Hue::Green}, {"bash", g::shell, Hue::Green}, {"zsh", g::shell, Hue::Green},
    {"fish", g::shell, Hue::Green}, {"ksh", g::shell, Hue::Green}, {"bashrc", g::shell, Hue::Green},
    {"zshrc", g::shell, Hue::Green}, {"profile", g::shell, Hue::Green},
    {"ps1", g::shell, Hue::Blue}, {"psm1", g::shell, Hue::Blue},
    {"bat", g::shell, Hue::Gray}, {"cmd", g::shell, Hue::Gray},
    // Web
    {"js", g::js, Hue::Yellow}, {"mjs", g::js, Hue::Yellow}, {"cjs", g::js, Hue::Yellow},
    {"ts", g::ts, Hue::Blue}, {"mts", g::ts, Hue::Blue}, {"cts", g::ts, Hue::Blue},
    {"d.ts", g::ts, Hue::Teal},
    {"jsx", g::react, Hue::Cyan}, {"tsx", g::react, Hue::Blue},
    {"html", g::html, Hue::Orange}, {"htm", g::html, Hue::Orange}, {"xhtml", g::html, Hue::Orange},
    {"vue", g::html, Hue::Green}, {"svelte", g::html, Hue::Red},
    {"css", g::css, Hue::Blue}, {"scss", g::css, Hue::Pink}, {"sass", g::css, Hue::Pink},
    {"less", g::css, Hue::Indigo},
    {"xml", g::xml, Hue::Orange}, {"xsd", g::xml, Hue::Orange}, {"xsl", g::xml, Hue::Orange},
    {"plist", g::xml, Hue::Gray}, {"ui", g::xml, Hue::Green},
    {"svg", g::svg, Hue::Yellow},
    // Data and config
    {"json", g::json, Hue::Yellow}, {"jsonc", g::json, Hue::Yellow}, {"json5", g::json, Hue::Yellow},
    {"jsonl", g::json, Hue::Yellow}, {"geojson", g::json, Hue::Yellow},
    {"toml", g::config, Hue::Brown}, {"yaml", g::config, Hue::Red}, {"yml", g::config, Hue::Red},
    {"ini", g::config, Hue::Gray}, {"cfg", g::config, Hue::Gray}, {"conf", g::config, Hue::Gray},
    {"properties", g::config, Hue::Gray}, {"desktop", g::config, Hue::Gray},
    {"env", g::key, Hue::Yellow},
    {"pem", g::key, Hue::Green}, {"key", g::key, Hue::Green}, {"pub", g::key, Hue::Green},
    {"crt", g::key, Hue::Green}, {"asc", g::key, Hue::Green}, {"gpg", g::key, Hue::Green},
    {"lock", g::lock, Hue::Gray},
    {"csv", g::table, Hue::Green}, {"tsv", g::table, Hue::Green}, {"xlsx", g::table, Hue::Green},
    {"ods", g::table, Hue::Green},
    {"sql", g::database, Hue::Yellow}, {"db", g::database, Hue::Gray}, {"sqlite", g::database, Hue::Gray},
    {"sqlite3", g::database, Hue::Gray},
    // Docs
    {"md", g::markdown, Hue::Blue}, {"markdown", g::markdown, Hue::Blue}, {"mdx", g::markdown, Hue::Blue},
    {"rst", g::doc, Hue::Blue}, {"adoc", g::doc, Hue::Blue}, {"org", g::doc, Hue::Teal},
    {"tex", g::doc, Hue::Green}, {"bib", g::doc, Hue::Green},
    {"txt", g::doc, Hue::Gray}, {"text", g::doc, Hue::Gray},
    {"pdf", g::doc, Hue::Red}, {"doc", g::doc, Hue::Blue}, {"docx", g::doc, Hue::Blue},
    {"odt", g::doc, Hue::Blue}, {"rtf", g::doc, Hue::Blue},
    {"log", g::log, Hue::Gray}, {"out", g::log, Hue::Gray},
    {"diff", g::diff, Hue::Green}, {"patch", g::diff, Hue::Green}, {"rej", g::diff, Hue::Red},
    // Build
    {"cmake", g::cmake, Hue::Green},
    {"mk", g::build, Hue::Orange}, {"mak", g::build, Hue::Orange}, {"ninja", g::build, Hue::Gray},
    {"gradle", g::build, Hue::Teal}, {"bazel", g::build, Hue::Green}, {"bzl", g::build, Hue::Green},
    {"dockerfile", g::docker, Hue::Cyan},
    // Shaders
    {"glsl", g::shader, Hue::Pink}, {"vert", g::shader, Hue::Pink}, {"frag", g::shader, Hue::Pink},
    {"comp", g::shader, Hue::Pink}, {"geom", g::shader, Hue::Pink}, {"tesc", g::shader, Hue::Pink},
    {"tese", g::shader, Hue::Pink}, {"hlsl", g::shader, Hue::Purple}, {"wgsl", g::shader, Hue::Teal},
    {"metal", g::shader, Hue::Gray}, {"spv", g::binary, Hue::Pink}, {"inc", g::hpp, Hue::Gray},
    // Media
    {"png", g::image, Hue::Purple}, {"jpg", g::image, Hue::Purple}, {"jpeg", g::image, Hue::Purple},
    {"gif", g::image, Hue::Purple}, {"bmp", g::image, Hue::Purple}, {"webp", g::image, Hue::Purple},
    {"ico", g::image, Hue::Purple}, {"icns", g::image, Hue::Purple}, {"tga", g::image, Hue::Purple},
    {"tif", g::image, Hue::Purple}, {"tiff", g::image, Hue::Purple}, {"psd", g::image, Hue::Blue},
    {"xcf", g::image, Hue::Gray}, {"avif", g::image, Hue::Purple}, {"heic", g::image, Hue::Purple},
    {"mp3", g::audio, Hue::Pink}, {"wav", g::audio, Hue::Pink}, {"flac", g::audio, Hue::Pink},
    {"ogg", g::audio, Hue::Pink}, {"opus", g::audio, Hue::Pink}, {"m4a", g::audio, Hue::Pink},
    {"mp4", g::video, Hue::Pink}, {"mkv", g::video, Hue::Pink}, {"webm", g::video, Hue::Pink},
    {"mov", g::video, Hue::Pink}, {"avi", g::video, Hue::Pink},
    {"ttf", g::font, Hue::Red}, {"otf", g::font, Hue::Red}, {"woff", g::font, Hue::Red},
    {"woff2", g::font, Hue::Red},
    // Archives and binaries
    {"zip", g::archive, Hue::Brown}, {"tar", g::archive, Hue::Brown}, {"gz", g::archive, Hue::Brown},
    {"tgz", g::archive, Hue::Brown}, {"xz", g::archive, Hue::Brown}, {"zst", g::archive, Hue::Brown},
    {"bz2", g::archive, Hue::Brown}, {"7z", g::archive, Hue::Brown}, {"rar", g::archive, Hue::Brown},
    {"deb", g::package, Hue::Red}, {"rpm", g::package, Hue::Red}, {"appimage", g::package, Hue::Blue},
    {"pkg.tar.zst", g::package, Hue::Blue}, {"msi", g::package, Hue::Blue}, {"nupkg", g::package, Hue::Blue},
    {"exe", g::binary, Hue::Gray}, {"dll", g::binary, Hue::Gray}, {"so", g::binary, Hue::Gray},
    {"dylib", g::binary, Hue::Gray}, {"a", g::binary, Hue::Gray}, {"lib", g::binary, Hue::Gray},
    {"o", g::binary, Hue::Gray}, {"obj", g::binary, Hue::Gray}, {"bin", g::binary, Hue::Gray},
    {"elf", g::binary, Hue::Gray}, {"wasm", g::binary, Hue::Indigo}, {"pyc", g::binary, Hue::Gray},
    {"iso", g::archive, Hue::Gray}, {"img", g::archive, Hue::Gray},
};

struct FolderRule {
    const char* key;
    uint32_t badge, badge_open;
    Hue hue;
};

#define BADGE(n) g::badge_##n, g::badge_open_##n
// Well-known folder names, lowercase.
const FolderRule kFolders[] = {
    {"src", BADGE(code), Hue::Blue}, {"source", BADGE(code), Hue::Blue}, {"sources", BADGE(code), Hue::Blue},
    {"lib", BADGE(code), Hue::Teal}, {"libs", BADGE(code), Hue::Teal}, {"app", BADGE(code), Hue::Blue},
    {"include", BADGE(h), Hue::Purple}, {"inc", BADGE(h), Hue::Purple}, {"headers", BADGE(h), Hue::Purple},
    {"test", BADGE(test), Hue::Green}, {"tests", BADGE(test), Hue::Green}, {"spec", BADGE(test), Hue::Green},
    {"specs", BADGE(test), Hue::Green}, {"__tests__", BADGE(test), Hue::Green}, {"testing", BADGE(test), Hue::Green},
    {"e2e", BADGE(test), Hue::Green}, {"fuzz", BADGE(test), Hue::Green}, {"benchmarks", BADGE(test), Hue::Green},
    {"docs", BADGE(book), Hue::Cyan}, {"doc", BADGE(book), Hue::Cyan}, {"documentation", BADGE(book), Hue::Cyan},
    {"man", BADGE(book), Hue::Cyan}, {"examples", BADGE(book), Hue::Teal}, {"samples", BADGE(book), Hue::Teal},
    {".git", BADGE(git), Hue::Red}, {".github", BADGE(git), Hue::Gray}, {".gitlab", BADGE(git), Hue::Orange},
    {"build", BADGE(build), Hue::Orange}, {"out", BADGE(build), Hue::Orange}, {"dist", BADGE(build), Hue::Orange},
    {"target", BADGE(build), Hue::Orange}, {"release", BADGE(build), Hue::Orange}, {"debug", BADGE(build), Hue::Orange},
    {"cmake-build-debug", BADGE(build), Hue::Orange}, {"cmake-build-release", BADGE(build), Hue::Orange},
    {"cmake", BADGE(build), Hue::Green}, {"ci", BADGE(build), Hue::Gray},
    {"assets", BADGE(image), Hue::Yellow}, {"images", BADGE(image), Hue::Yellow}, {"img", BADGE(image), Hue::Yellow},
    {"icons", BADGE(image), Hue::Yellow}, {"media", BADGE(image), Hue::Yellow}, {"res", BADGE(image), Hue::Yellow},
    {"resources", BADGE(image), Hue::Yellow}, {"static", BADGE(image), Hue::Yellow},
    {"public", BADGE(image), Hue::Yellow}, {"logo", BADGE(image), Hue::Yellow}, {"textures", BADGE(image), Hue::Yellow},
    {"scripts", BADGE(shell), Hue::Green}, {"script", BADGE(shell), Hue::Green}, {"bin", BADGE(binary), Hue::Gray},
    {"tools", BADGE(gear), Hue::Gray}, {"utils", BADGE(gear), Hue::Gray}, {".vscode", BADGE(gear), Hue::Blue},
    {".idea", BADGE(gear), Hue::Gray}, {".claude", BADGE(gear), Hue::Orange},
    {"config", BADGE(config), Hue::Gray}, {".config", BADGE(config), Hue::Gray}, {"configs", BADGE(config), Hue::Gray},
    {"settings", BADGE(config), Hue::Gray}, {"etc", BADGE(config), Hue::Gray},
    {"node_modules", BADGE(package), Hue::Green}, {"vendor", BADGE(package), Hue::Brown},
    {"third_party", BADGE(package), Hue::Brown}, {"thirdparty", BADGE(package), Hue::Brown},
    {"external", BADGE(package), Hue::Brown}, {"deps", BADGE(package), Hue::Brown},
    {"packages", BADGE(package), Hue::Brown}, {"packaging", BADGE(package), Hue::Blue},
    {"fonts", BADGE(font), Hue::Red}, {"shaders", BADGE(shader), Hue::Pink},
    {"docker", BADGE(docker), Hue::Cyan}, {".devcontainer", BADGE(docker), Hue::Cyan},
    {"archive", BADGE(archive), Hue::Brown}, {"backup", BADGE(archive), Hue::Brown},
};
#undef BADGE

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

template <typename R>
std::unordered_map<std::string, const R*> index_of(const R* rules, size_t n) {
    std::unordered_map<std::string, const R*> m;
    for (size_t i = 0; i < n; i++) m.emplace(rules[i].key, &rules[i]);
    return m;
}

} // namespace

FileIcon for_file(std::string_view name) {
    static const auto names = index_of(kNames, std::size(kNames));
    static const auto exts = index_of(kExtensions, std::size(kExtensions));
    std::string n = lower(name);
    if (auto it = names.find(n); it != names.end()) return {it->second->mark, it->second->hue};
    // Longest extension first: "a.d.ts" tries "d.ts" before "ts". A leading dot is part of
    // the name, so ".bashrc" looks up "bashrc".
    for (size_t dot = n.find('.'); dot != std::string::npos; dot = n.find('.', dot + 1)) {
        if (dot + 1 >= n.size()) break;
        if (auto it = exts.find(n.substr(dot + 1)); it != exts.end()) return {it->second->mark, it->second->hue};
    }
    for (const Rule& r : kPrefixes)
        if (n.rfind(r.key, 0) == 0) return {r.mark, r.hue};
    return {g::doc, Hue::Gray};
}

FolderIcon for_folder(std::string_view name) {
    static const auto folders = index_of(kFolders, std::size(kFolders));
    if (auto it = folders.find(lower(name)); it != folders.end())
        return {it->second->badge, it->second->badge_open, it->second->hue};
    return {};
}

Style parse_style(std::string_view s) {
    if (s == "minimal") return Style::Minimal;
    if (s == "none") return Style::None;
    return Style::Overheat;
}

const char* style_name(Style s) {
    switch (s) {
        case Style::Minimal: return "minimal";
        case Style::None: return "none";
        default: return "overheat";
    }
}

} // namespace oh::file_icons
