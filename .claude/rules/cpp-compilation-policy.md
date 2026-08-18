# C++ Compilation Policy

This policy applies to all VERITAS source code and is enforced through CMake configuration and code review.

## No RTTI

VERITAS builds with RTTI (Run-Time Type Information) disabled. This is a project-wide constraint inherited from LLVM, which also builds with `-fno-rtti`.

### Rationale

- **LLVM compatibility** — VERITAS links against LLVM/Clang libraries that are built without RTTI. Mixing RTTI-enabled code with RTTI-disabled libraries causes linker errors and undefined behavior.
- **Binary size** — RTTI adds vtable overhead and type metadata to every polymorphic class.
- **Performance** — RTTI prevents certain compiler optimizations.

### Rules

1. Never use `dynamic_cast<T>` or `typeid()`. These operators require RTTI and will fail to compile.
2. Use LLVM's `llvm::isa<T>()`, `llvm::cast<T>()`, and `llvm::dyn_cast<T>()` for safe downcasting with manual RTTI via `classof()` methods.
3. For VERITAS-owned class hierarchies, implement manual type discrimination:
   - Add an enum-based type tag to the base class
   - Implement `classof()` static methods for LLVM-style casting
   - Document the type hierarchy in the header

### Example: Manual RTTI

```cpp
class Expr {
 public:
  enum class Kind { Literal, BinaryOp, Call };
  
  Kind getKind() const { return kind_; }
  
  // LLVM-style casting support
  static bool classof(const Expr*) { return true; }
  
 protected:
  explicit Expr(Kind k) : kind_(k) {}
  
 private:
  Kind kind_;
};

class LiteralExpr : public Expr {
 public:
  LiteralExpr() : Expr(Kind::Literal) {}
  
  static bool classof(const Expr* e) {
    return e->getKind() == Kind::Literal;
  }
};

// Usage
if (llvm::isa<LiteralExpr>(expr)) {
  auto* literal = llvm::cast<LiteralExpr>(expr);
  // ...
}
```

## No Exceptions

VERITAS builds with exceptions disabled (`-fno-exceptions`). This is also inherited from LLVM's build configuration.

### Rationale

- **LLVM compatibility** — LLVM libraries are built without exception support. Throwing across LLVM boundaries causes undefined behavior.
- **Deterministic control flow** — Exception-free code has explicit error paths visible in the function signature and call sites.
- **Performance** — Exception tables add binary size overhead even when no exceptions are thrown.

### Rules

1. Never use `throw`, `try`, `catch`, or exception specifications. These will fail to compile.
2. Never use standard library facilities that throw exceptions:
   - `std::vector::at()` → use `operator[]` with bounds checks
   - `std::map::at()` → use `find()` or `operator[]`
   - `std::stoi()` / `std::stod()` → use custom parsing with error codes
   - Dynamic allocation that throws `std::bad_alloc` → custom allocators or checks

3. Use `veritas::Status` and `veritas::StatusOr<T>` for all fallible operations:
   ```cpp
   veritas::StatusOr<FunctionSummary> ExtractSummary(const Function* f);
   
   auto result = ExtractSummary(func);
   if (!result.ok()) {
     return result.status();  // Propagate error
   }
   FunctionSummary summary = std::move(result).value();
   ```

4. For truly unrecoverable errors (internal invariant violations, logic bugs), use:
   - `assert()` in debug builds
   - `llvm::report_fatal_error()` in production builds
   - `std::abort()` when neither is appropriate

5. Document error conditions in function comments. Every fallible function must state its failure modes.

### Exception: Test Code

Test fixtures (`tests/support/`) may use `std::abort()` for setup failures since tests are optional and run in a controlled environment. Never use exceptions in test code.

### Migration Note

When porting code that uses exceptions:
- Replace `throw` with early returns of `Status`
- Replace `try/catch` with explicit status checking
- Replace RAII exception guarantees with explicit cleanup or scope guards

## Verification

CMake enforces these policies via compiler flags:

```cmake
target_compile_options(target PRIVATE -fno-rtti -fno-exceptions)
```

Both flags are inherited from `LLVM_DEFINITIONS` and applied project-wide through `VeritasLLVM.cmake`.

### Pre-commit Check

Before committing code that includes RTTI or exception usage, this grep will catch it:

```bash
git diff --cached | grep -E '(dynamic_cast|typeid\(|\bthrow\b|\btry\b|\bcatch\b)'
```

Exit code 0 means violations were found. Fix them before committing.

## Related Documentation

- LLVM Programmer's Manual: [The LLVM-style RTTI](https://llvm.org/docs/ProgrammersManual.html#the-llvm-style-rtti)
- `docs/third_party/LLVM.md` — VERITAS/LLVM build contract
- `include/veritas/core/Status.h` — Error handling primitives
