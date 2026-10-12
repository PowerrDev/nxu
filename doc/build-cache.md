# Build caching

The ARM64 Makefile automatically uses ccache for kernel and userland C/assembly
compilation when it is installed. Without ccache, it uses clang directly.
The cache lives in `.build-cache/ccache`, outside BUILD, so `make clean` keeps
compiled results available for the next build. Source contents, headers, compiler
options and compiler identity are checked by ccache before reuse.

Use the usual commands:

```sh
make -j8 all
make -j8 userland
```

Inspect the local cache with:

```sh
CCACHE_DIR="$PWD/.build-cache/ccache" ccache --show-stats
```

Use `make CACHE=0` to bypass compiler caching, or set `CCACHE_DIR` to use an
existing cache. Explicit `CC` and `USER_CC` command-line overrides still work.
Linking, signing and disk-image staging are not compiler-cache operations.

The isolated GUI builder preserves timestamps for unchanged adapted Rust sources,
generated headers and freestanding sysroot inputs. Cargo can reuse its existing
artifacts on repeated builds instead of recompiling after every source copy.
Changed files are replaced and deleted files are removed from the isolated tree.
Sibling framework repositories remain untouched. Rust artifacts still live under
BUILD and are removed by `make clean`; prefer incremental builds during development.
