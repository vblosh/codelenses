#include <map>
#include <string>
#include <vector>

template <typename T>
class Stack {
public:
    void push(const T& val);
    T pop();
    bool empty() const { return elements.empty(); }
    std::size_t size() const { return elements.size(); }

private:
    std::vector<T> elements;
};

template <typename T>
void Stack<T>::push(const T& val) {
    elements.push_back(val);
}

template <typename T, typename U>
auto pair_sum(T a, U b) -> decltype(a + b) {
    return a + b;
}

template <typename T>
using StringMap = std::map<std::string, T>;

template <typename T>
struct TypeTraits {
    static constexpr bool is_pointer = false;
};

template <typename T>
struct TypeTraits<T*> {
    static constexpr bool is_pointer = true;
};

void test_templates() {
    Stack<int> s;
    s.push(42);
    auto total = pair_sum(10, 3.14);
    StringMap<int> mapping;
}
