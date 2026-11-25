// Randomized check of the piece table against a plain std::string model,
// including undo/redo round-trips. Exits non-zero on the first divergence.
#include "editor/buffer.h"
#include <cstdio>
#include <random>

using oh::TextBuffer;

static bool check(const TextBuffer& b, const std::string& model, const char* what, int iter) {
    if (b.text() == model) return true;
    std::fprintf(stderr, "MISMATCH after %s (iteration %d)\n  got:  [%s]\n  want: [%s]\n",
                 what, iter, b.text().c_str(), model.c_str());
    return false;
}

int main(int argc, char** argv) {
    unsigned seed = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1;
    std::mt19937 rng(seed);
    const char* alphabet = "ab\ncd\xCF\x89 ";
    for (int round = 0; round < 300; round++) {
        TextBuffer b;
        std::string model = "hello\nworld\n";
        b.load_string(model);
        std::vector<std::string> history{model};
        int groups = 1 + static_cast<int>(rng() % 6);
        for (int g = 0; g < groups; g++) {
            b.begin_undo_group();
            int ops = 1 + static_cast<int>(rng() % 4);
            for (int k = 0; k < ops; k++) {
                if (model.empty() || rng() % 2) {
                    size_t pos = rng() % (model.size() + 1);
                    std::string s;
                    for (size_t n = 1 + rng() % 3; n; n--) s.push_back(alphabet[rng() % 8]);
                    b.insert(pos, s);
                    model.insert(pos, s);
                } else {
                    size_t pos = rng() % model.size();
                    size_t len = 1 + rng() % std::min<size_t>(4, model.size() - pos);
                    b.erase(pos, len);
                    model.erase(pos, len);
                }
                if (!check(b, model, "edit", round)) return 1;
            }
            b.end_undo_group();
            history.push_back(model);
        }
        for (int g = groups; g > 0; g--) {
            b.undo();
            if (!check(b, history[g - 1], "undo", round)) return 1;
        }
        for (int g = 1; g <= groups; g++) {
            b.redo();
            if (!check(b, history[g], "redo", round)) return 1;
        }
    }
    std::puts("buffer_fuzz: ok");
    return 0;
}
