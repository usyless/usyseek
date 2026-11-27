# AGENTS.md

Guidelines for working in the `usyseek` repository (C++23 + Rust).

## Shared Library (`usylibpp`)

- Many shared components used by this app live in `usylibpp`.
- If a task requires changes to `usylibpp`, make those changes directly in the `usylibpp` repository, then commit and push them there.
- If new reusable code or utilities would fit well in `usylibpp`, place them there instead of duplicating them in this repo, and commit and push the changes in `usylibpp`.

## Versioning

- **usyseek version**: Bump `VERSION` in `project(usyseek VERSION ...)` in `CMakeLists.txt` — nowhere else.
- **Rust crate version**: Bump `version` in `rust/Cargo.toml` — nowhere else.
  - Only bump the Rust version when making changes to the Rust codebase (`rust/`).
  - Check published releases (GitHub tags) before bumping: do not bump if the current version has not yet been in a published release.

## Static Analysis (`clang-tidy`)

- After making C/C++ edits, verify that your changes do not introduce any `clang-tidy` warnings (rules are defined in `.clang-tidy`).
- Keep warnings to a minimum. Acceptable exceptions are:
  - False positives on Windows API structs.
  - Intentional, verified-safe performance choices (e.g., skipping zero-initialization of an array).
- When suppressing a verified-valid warning, scope it to the specific check (e.g., `// NOLINT(cppcoreguidelines-pro-type-member-init)`). Use `NOLINT` sparingly — do not scatter blanket suppressions across the codebase.

## Code Style & Performance

- Match the style, idioms, and conventions of the surrounding codebase.
- Write code with performance and allocation efficiency in mind:
  - Minimize heap allocations where practical (e.g., reuse buffers, use fixed-size stack arrays when sizes are bounded).
  - Prefer the stack-allocated string helpers in `include/usyseek/string_helpers.hpp` over allocating standard utilities — e.g., `usyseek::to_fixed_str` / `usyseek::to_fixed_wstr` instead of `std::to_string` / `std::to_wstring`, and `FixedString` / `FixedWString` for bounded stack strings.
  - Avoid `std::string + std::string` concatenation. Use `usylibpp::strings::concat_strings` when constructing a new string from multiple parts, and `usyseek::append_to` (or pre-reserved in-place appending) when appending to an existing string.
  - Continue this pattern when adding new utilities or code paths: prefer non-allocating or single-allocation helpers over chained temporaries.
- Never trade away correctness for micro-optimizations: keep **thread safety** and **avoiding undefined behavior** strictly in mind when reusing buffers, skipping initialization, or manipulating low-level memory.
