# DBPlayground

A platform to show/verify how to integrate other interesting things with a database.

Based on CMU 15445's course project.

## Build

```bash
git clone git@github.com:zhiqiang-hhhh/DBPlayground.git
cd DBPlayground
git submodule update --init --recursive
cmake -S . -B build -DDBPLAYGROUND_BUILD_TESTS=ON
cmake --build build --target dbplayground -j
```

Build and run an individual test:

```bash
cmake --build build --target BPlusTreeTest -j
./build/test/BPlusTreeTest
```

## Format

Use clang-format to auto format

```bash
cmake --build build --target format
```
