// Test fixture for macro expansion tracking

#define CALL_TARGET(x) helper(x)

int helper(int x) {
  return x + 1;
}

int caller(int x) {
  return CALL_TARGET(x);
}
