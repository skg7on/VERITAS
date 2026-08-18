// Test fixture for template functions

template <typename T>
T add(T a, T b) {
  return a + b;
}

template <>
int add<int>(int a, int b) {
  return a + b + 1;
}

template <typename T>
class Container {
 public:
  void insert(const T& value) {
    // Template member function
  }
};
