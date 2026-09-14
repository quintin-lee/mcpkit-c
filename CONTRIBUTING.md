# Contributing

## Workflow

1. Create a feature branch from `master` (`phase<n>-<topic>`).
2. Keep the layering direction green: apps -> server/client ->
   protocol -> core. New external dependencies need explicit approval.
3. Public structs stay opaque (`typedef struct mcp_foo mcp_foo_t;`).
   Never expose `pthread_*`, `uv_*`, or backend handles in headers.
4. C23 only. `CMAKE_C_EXTENSIONS OFF` must stay off.
5. Every behavior change ships with a test; run the full gate:

```sh
cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Commits

Conventional Commits: `feat:`, `fix:`, `build:`, `test:`, `docs:`,
`chore:`. One logical change per commit.
