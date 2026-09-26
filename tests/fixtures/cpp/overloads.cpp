int add(int a, int b) {
    return a + b;
}

double add(double a, double b) {
    return a + b;
}

int add(int a, int b, int c) {
    return a + b + c;
}

class Renderer {
public:
    void render() { draw_frame(1.0f); }

    void render(float scale) { draw_frame(scale); }

    void render(float scale, bool wireframe) const {
        if (wireframe) {
            draw_wireframe(scale);
        } else {
            draw_frame(scale);
        }
    }

private:
    void draw_frame(float scale) const;
    void draw_wireframe(float scale) const;
};

void run_overloads() {
    int sum1 = add(1, 2);
    double sum2 = add(1.5, 2.5);
    int sum3 = add(1, 2, 3);

    Renderer r;
    r.render();
    r.render(2.0f);
    r.render(2.0f, true);
}
