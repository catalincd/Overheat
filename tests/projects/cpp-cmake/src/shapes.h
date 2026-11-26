#pragma once
#include <string>
#include <vector>

struct Shape {
    std::string name;
    double area;
};

std::vector<Shape> make_shapes();

template <typename T>
double total_area(const std::vector<T>& items) {
    double sum = 0;
    for (const auto& it : items) sum += it.area;
    return sum;
}
