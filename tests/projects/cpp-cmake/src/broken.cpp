// Intentional errors for Overheat's problem matchers (configure with -DBROKEN=ON).
#include "shapes.h"

struct Label {
    std::string text;  // no `area` member: total_area<Label> fails inside shapes.h
};

int broken_unused(int value) {
    int unused = 0;
    std::vector<Label> labels{{"a"}, {"b"}};
    double a = total_area(labels);
    return value + a + missing_name;
}
