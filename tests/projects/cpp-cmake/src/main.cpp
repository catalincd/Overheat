#include <cstdio>
#include "shapes.h"

int main() {
    auto shapes = make_shapes();
    std::printf("%zu shapes, total area %.2f\n", shapes.size(), total_area(shapes));
    return 0;
}
