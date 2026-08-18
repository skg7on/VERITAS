// Test fixture for internal linkage in file A

static int internal_helper() {
  return 42;
}

int public_function_a() {
  return internal_helper();
}
