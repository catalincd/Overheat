// Round-trips workspace files through the TOML writer/reader and checks the store.
#include "workspace.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace oh;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char** argv) {
    Workspace ws;
    ws.name = "My \"proj\"";
    ws.folders = {"/home/u/proj", "ssh://devbox/srv/app", "/tmp/with space"};
    ws.split = true;
    ws.split_ratio = 0.4f;
    ws.active_group = 1;
    ws.sidebar = false;
    ws.terminal = false;
    ws.sidebar_view = "scm";
    ws.settings = {{"font_size", "14"}, {"theme", "\"Overheat Light\""}, {"word_wrap", "true"}};
    ws.last_opened = 1764150000;
    ws.tabs = {{"src/main.cpp", 0, 10, 4, 220.5f, true}, {"/etc/hosts", 1, 0, 0, 0, false}};

    std::string text = WorkspaceStore::serialize(ws);
    Workspace back;
    std::string err;
    CHECK(WorkspaceStore::parse(text, back, &err));
    CHECK(back.name == ws.name);
    CHECK(back.folders == ws.folders);
    CHECK(back.split && back.active_group == 1 && !back.sidebar && !back.terminal);
    CHECK(back.sidebar_view == "scm");
    CHECK(back.split_ratio > 0.39f && back.split_ratio < 0.41f);
    CHECK(back.settings == ws.settings);
    CHECK(back.last_opened == ws.last_opened);
    CHECK(back.tabs.size() == 2);
    CHECK(back.tabs[0].path == "src/main.cpp" && back.tabs[0].line == 10 && back.tabs[0].col == 4 && back.tabs[0].active);
    CHECK(back.tabs[1].group == 1 && back.tabs[1].path == "/etc/hosts");

    // Hand-written file: comments, single-line array, unknown keys, literal strings.
    Workspace hand;
    CHECK(WorkspaceStore::parse("# hi\nname = 'x'  # c\nfolders = [\"/a\", '/b'] # two\nfuture = 3\n[future]\nk = [1,\n 2]\n"
                                "[[tabs]]\npath = \"f.txt\"\n", hand, &err));
    CHECK(hand.name == "x" && hand.folders.size() == 2 && hand.folders[1] == "/b" && hand.tabs.size() == 1);
    Workspace bad;
    CHECK(!WorkspaceStore::parse("folders = [\"/a\"\n", bad, &err));

    auto ssh = parse_ssh_folder("ssh://me@box:2222/home/me/x");
    CHECK(ssh && ssh->first == "me@box:2222" && ssh->second == "/home/me/x");
    CHECK(!parse_ssh_folder("/local"));
    CHECK(ssh_folder("box", "/a/b") == "ssh://box/a/b");
    CHECK(WorkspaceStore::sanitize_name("  a/b:c. ") == "a-b-c");

    // Store on disk: save, list by recency, containing(), project file, remove.
    fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "overheat-workspace-test";
    fs::remove_all(dir);
    WorkspaceStore store;
    store.set_config_dir(dir);
    Workspace a = ws, b;
    a.name = "alpha";
    a.last_opened = 100;
    b.name = "beta";
    b.folders = {"/home/u/proj/"};
    b.last_opened = 200;
    CHECK(store.save(a) && store.save(b));
    auto list = store.list();
    CHECK(list.size() == 2 && list[0].name == "beta" && list[1].name == "alpha");
    CHECK(store.containing("/home/u/proj").size() == 2);
    CHECK(store.containing("ssh://devbox/srv/app").size() == 1);
    CHECK(store.containing("/home/u").empty());
    fs::create_directories(dir / "proj");
    std::ofstream(dir / "proj" / WorkspaceStore::PROJECT_FILE) << "folders = [\".\", \"../other\"]\n[settings]\nminimap = false\n";
    auto pf = WorkspaceStore::load_project_file(dir / "proj" / WorkspaceStore::PROJECT_FILE);
    CHECK(pf && pf->name == "proj" && pf->folders.size() == 2);
    CHECK(pf && pf->folders[0] == (dir / "proj").lexically_normal().generic_string());
    CHECK(pf && pf->folders[1] == (dir / "other").lexically_normal().generic_string());
    CHECK(pf && pf->settings.size() == 1);
    CHECK(store.remove("alpha") && store.list().size() == 1);
    // A file whose `name` differs from its file name is still found, saved in place and removed.
    std::ofstream(dir / "workspaces" / "odd.toml") << "name = \"Odd One\"\nfolders = [\"/x\"]\n";
    auto odd = store.load("Odd One");
    CHECK(odd && odd->file.filename() == "odd.toml");
    CHECK(odd && store.save(*odd) && !fs::exists(store.path_for("Odd One")));
    CHECK(store.exists("Odd One") && store.remove("Odd One") && !fs::exists(dir / "workspaces" / "odd.toml"));
    fs::remove_all(dir);

    if (failures) return 1;
    std::printf("workspace_test: ok\n");
    return 0;
}
