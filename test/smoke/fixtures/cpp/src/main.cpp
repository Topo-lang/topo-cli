// Host fixture for the topo-init scaffold smoke: a namespaced free
// function plus a constructor-free class (members defined inline), and a
// main() so the scaffolded project links as an executable.

namespace calc {

int add(int a, int b) { return a + b; }

class Accumulator {
public:
    void store(int value) { value_ = value; }
    int stored() const { return value_; }

private:
    int value_ = 0;
};

} // namespace calc

int main() {
    calc::Accumulator acc;
    acc.store(calc::add(2, 2));
    return acc.stored() == 4 ? 0 : 1;
}
