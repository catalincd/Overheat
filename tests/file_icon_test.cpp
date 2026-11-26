// Checks the file name -> icon table (src/ui/file_icon_map.cpp).
#include "ui/file_icons.h"
#include "ui/file_icon_glyphs.h"
#include <cstdio>

using namespace oh;
namespace g = oh::icon_glyph;

static int failures = 0;

static void expect_file(const char* name, uint32_t mark) {
    auto fi = file_icons::for_file(name);
    if (fi.mark != mark) {
        std::fprintf(stderr, "FAIL %s: got 0x%X, want 0x%X\n", name, fi.mark, mark);
        failures++;
    }
}

static void expect_folder(const char* name, uint32_t badge) {
    auto fi = file_icons::for_folder(name);
    if (fi.badge != badge) {
        std::fprintf(stderr, "FAIL folder %s: got 0x%X, want 0x%X\n", name, fi.badge, badge);
        failures++;
    }
}

int main() {
    expect_file("main.c", g::c);
    expect_file("app.cpp", g::cpp);
    expect_file("APP.CPP", g::cpp);
    expect_file("app.h", g::h);
    expect_file("types.hpp", g::hpp);
    expect_file("lib.rs", g::rust);
    expect_file("Cargo.toml", g::rust);
    expect_file("settings.toml", g::config);
    expect_file("index.d.ts", g::ts);
    expect_file("index.ts", g::ts);
    expect_file("App.tsx", g::react);
    expect_file("CMakeLists.txt", g::cmake);
    expect_file("notes.txt", g::doc);
    expect_file("Makefile", g::build);
    expect_file("Dockerfile", g::docker);
    expect_file("Dockerfile.dev", g::docker);
    expect_file(".gitignore", g::git);
    expect_file(".bashrc", g::shell);
    expect_file(".env.local", g::key);
    expect_file("README.md", g::markdown);  // extension wins over the prefix...
    expect_file("README", g::readme);       // ...and the prefix catches the rest
    expect_file("LICENSE", g::license);
    expect_file("Cargo.lock", g::lock);
    expect_file("yarn.lock", g::lock);
    expect_file("logo.png", g::image);
    expect_file("src.tar.gz", g::archive);
    expect_file("overheat-0.2.0-1-x86_64.pkg.tar.zst", g::package);
    expect_file("quad.frag.glsl", g::shader);
    expect_file("no_extension", g::doc);
    expect_file("trailing.", g::doc);
    expect_file("", g::doc);
    expect_folder("src", g::badge_code);
    expect_folder("Tests", g::badge_test);
    expect_folder("docs", g::badge_book);
    expect_folder(".git", g::badge_git);
    expect_folder("build", g::badge_build);
    expect_folder("assets", g::badge_image);
    expect_folder("random", 0);
    if (file_icons::parse_style("none") != file_icons::Style::None ||
        file_icons::parse_style("bogus") != file_icons::Style::Overheat) {
        std::fprintf(stderr, "FAIL parse_style\n");
        failures++;
    }
    if (failures == 0) std::printf("file_icon_test: ok\n");
    return failures ? 1 : 0;
}
