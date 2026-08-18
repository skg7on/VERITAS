// Test fixture for internal linkage in file B

static int internal_helper() {
  return 99;
}

int public_function_b() {
  return internal_helper();
}
