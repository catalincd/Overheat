// Shapes in a scene, sorted by area.
#include <algorithm>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace demo {

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::string name() const = 0;
};

struct Circle final : Shape {
    explicit Circle(double r) : radius(r) {}
    double area() const override { return std::numbers::pi * radius * radius; }
    std::string name() const override { return "circle"; }
    double radius;
};

struct Rect final : Shape {
    Rect(double w, double h) : width(w), height(h) {}
    double area() const override { return width * height; }
    std::string name() const override { return "rect"; }
    double width, height;
};

template <typename T, typename... Args>
void add(std::vector<std::unique_ptr<Shape>>& scene, Args&&... args) {
    scene.push_back(std::make_unique<T>(std::forward<Args>(args)...));
}

} // namespace demo

int main() {
    std::vector<std::unique_ptr<demo::Shape>> scene;
    demo::add<demo::Circle>(scene, 1.5);
    demo::add<demo::Rect>(scene, 2.0, 3.0);
    std::ranges::sort(scene, {}, [](const auto& s) { return s->area(); });
    return scene.empty() ? 1 : 0;
}
