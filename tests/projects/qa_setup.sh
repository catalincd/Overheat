#!/bin/sh
# Copies the sample projects to a scratch folder for the tools/qa/tasks_*.txt scripts and
# makes each one's broken variant the default build (.overheat/tasks.toml):
#   tests/projects/qa_setup.sh /tmp/oh-qa
#   OVERHEAT_BIN=build/overheat QA_CWD=/tmp/oh-qa/c \
#       tools/qa/drive.py 1400 900 tools/qa/tasks_c.txt /tmp/oh-qa/out-c /tmp/oh-qa/c/main.c
#   (tasks_rust.txt: QA_CWD=/tmp/oh-qa/rust-cargo, file src/main.rs;
#    tasks_cmake.txt: QA_CWD=/tmp/oh-qa/cpp-cmake, file src/main.cpp)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dest=${1:?usage: qa_setup.sh <scratch dir>}
mkdir -p "$dest"
for p in c cpp-cmake rust-cargo; do
    rm -rf "${dest:?}/$p"
    cp -r "$here/$p" "$dest/$p"
done
mkdir -p "$dest/c/.overheat" "$dest/rust-cargo/.overheat"
cat > "$dest/c/.overheat/tasks.toml" <<'EOF'
[[task]]
label = "make broken"
command = "make BROKEN=1"
group = "build"
default = true
matcher = ["gcc"]

[[task]]
label = "slow"
command = "echo starting; sleep 30; echo never"
EOF
cat > "$dest/rust-cargo/.overheat/tasks.toml" <<'EOF'
[[task]]
label = "cargo build (broken)"
program = "cargo"
args = ["build", "--features", "broken", "--message-format=json-diagnostic-rendered-ansi"]
group = "build"
default = true
matcher = ["cargo-json", "rustc"]
EOF
echo "sample projects ready in $dest"
