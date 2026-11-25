#!/usr/bin/env bash
# make_test_repo.sh DIR — throwaway repo for git UI tests: tracked/modified/untracked
# files, a feature branch, and an upstream so the branch is 1 ahead / 1 behind.
set -euo pipefail
dir=$(realpath -m "$1")
rm -rf "$dir"; mkdir -p "$dir"
cfg=(-c user.name="QA Bot" -c user.email=qa@example.invalid -c init.defaultBranch=main -c advice.detachedHead=false)
git "${cfg[@]}" init -q --bare "$dir/origin.git"
git "${cfg[@]}" clone -q "$dir/origin.git" "$dir/work" 2>/dev/null
cd "$dir/work"
git config user.name "QA Bot"; git config user.email qa@example.invalid
mkdir -p src
printf '#include <stdio.h>\n\nint add(int a, int b) {\n    return a + b;\n}\n\nint sub(int a, int b) {\n    return a - b;\n}\n\nint main(void) {\n    printf("%%d\\n", add(1, 2));\n    return 0;\n}\n' > src/main.c
printf '# Test repo\n\nUsed by Overheat QA.\n' > README.md
git add -A; git commit -q -m "Initial commit"
git push -q origin HEAD:main 2>/dev/null; git branch -q --set-upstream-to=origin/main
git checkout -q -b feature/login; echo "login" > login.txt; git add login.txt; git commit -q -m "Add login stub"; git checkout -q main
# Someone else pushes (behind by 1)
git clone -q "$dir/origin.git" "$dir/other" 2>/dev/null
(cd "$dir/other" && git config user.name "Other Dev" && git config user.email other@example.invalid &&
 echo "notes" > NOTES.md && git add NOTES.md && git commit -q -m "Add notes" && git push -q origin HEAD:main 2>/dev/null)
git fetch -q origin
# Local commit not pushed (ahead by 1)
printf 'int mul(int a, int b) { return a * b; }\n' > src/mul.c; git add src/mul.c; git commit -q -m "Add mul"
# Working tree changes: one staged, one modified, one untracked
printf '# Test repo\n\nUsed by Overheat QA tests.\n' > README.md; git add README.md
echo "scratch" > untracked.txt
echo "$dir/work"
