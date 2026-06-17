# phpatcher

A PHP extension that transparently applies a **unified diff (git patch)** to PHP
source files **in memory**, right before they are compiled — without ever
modifying the files on disk.

## What problem does it solve?

Sometimes you need to change how some PHP code behaves, but you are not allowed
(or do not want) to edit the files themselves:

- third-party / vendored code you must not fork,
- read-only or immutable deployments (containers, signed images, `noexec`/RO
  filesystems),
- audited or integrity-checked code where every on-disk byte must stay as
  shipped,
- hotfixing production behaviour without touching the release artifact.

phpatcher lets you keep the original files exactly as they are on disk and still
run patched code. You supply a patch file once; the extension applies it in
memory to every matching file the engine compiles. The result:

- The files on disk are **never** written to.
- Works for `include`, `require`, `include_once`, `require_once`, the main
  script, and anything else that goes through PHP's compiler.
- Implemented in **C++17** for speed; the patch index is built once at startup.
- Coexists with **OPcache**: the patched source is what OPcache compiles and
  caches, so there is zero per-request cost once warmed.
- Builds and runs on **macOS** and **Linux**.

## How it works

PHP exposes a documented, overridable function pointer, `zend_compile_file`,
that the engine calls for every file it compiles. OPcache, Xdebug, Blackfire and
many other extensions hook it. phpatcher does the same:

1. At module startup (`MINIT`) it reads the configured patch file, parses every
   file diff and indexes them by canonical absolute path (a hash map for O(1)
   lookup).
2. It installs its `zend_compile_file` hook at the head of the chain (always
   calling the previous handler, e.g. OPcache, afterwards).
3. When the engine is about to compile a file, the hook resolves the file's real
   path. If a patch exists for it, phpatcher hands the patched bytes to the
   compiler via the file handle's in-memory buffer (`zend_file_handle.buf`). The
   on-disk file is left untouched. By default the patched bytes are prepared once
   at startup (`phpatcher.precompute`), so the hot path is a lock-free lookup; see
   [Memory model](#memory-model-php-fpm).

A fast, syscall-free pre-filter on the file's basename means files that are not
in the patch pay only a single hash lookup — no `realpath()`, no I/O.

> **Note:** this hooks the engine's extension API — it does **not** modify,
> recompile or patch the PHP interpreter itself. Nothing in php-src is changed.

## Requirements

- PHP 8.0+ (developed and tested against PHP 8.5). The `zend_file_handle.buf`
  in-memory buffer mechanism this relies on exists since PHP 8.0.
- A C++17 compiler (clang or gcc).
- `phpize` / `php-config` (the PHP development headers).

## Build & install

```bash
phpize
./configure --enable-phpatcher
make
make test            # runs the .phpt integration suite
sudo make install    # optional: install into the PHP extension dir
```

`phpize` generates the build system from `config.m4`; `make` produces
`modules/phpatcher.so` (a C++17 build). `make install` copies it into the
directory reported by `php-config --extension-dir`.

## Enabling the extension

Load it and point it at a patch file in your `php.ini` (or via `-d` flags):

```ini
extension=phpatcher.so

; Path to the unified-diff / git patch file.
phpatcher.patch_file=/path/to/changes.patch

; Base directory the patch's a/ b/ paths are resolved against
; (usually your project / repository root). Defaults to the CWD.
phpatcher.base_dir=/path/to/project

; Optional toggles (all default as shown):
phpatcher.enabled=1     ; master on/off switch
phpatcher.strict=1      ; emit an E_WARNING when a patch fails to apply
phpatcher.precompute=1  ; patch all targets at startup (shared via copy-on-write)
phpatcher.cache=1       ; lazily cache patched contents per process
```

With no `phpatcher.patch_file` configured the extension installs no hook and
stays completely silent, so it is safe to load globally.

Verify it is active:

```bash
php -m | grep phpatcher          # listed among loaded modules
php --ri phpatcher               # version, active state, indexed file count
```

## Configuration reference

All settings are `PHP_INI_SYSTEM` (read once at startup, set them in `php.ini`).

| Setting | Default | Description |
| --- | --- | --- |
| `phpatcher.patch_file` | `""` | Path to the unified-diff file to apply. |
| `phpatcher.base_dir` | CWD | Root used to resolve the patch's relative `a/`,`b/` paths to absolute files. |
| `phpatcher.enabled` | `1` | Master switch. When `0`, no hook is installed. |
| `phpatcher.strict` | `1` | When `1`, a patch that does not cleanly apply (or whose file cannot be read) emits an `E_WARNING`. In all cases phpatcher *fails safe*: the original, unmodified file is compiled. |
| `phpatcher.precompute` | `1` | Patch **every** indexed file once at `MINIT`. Under a forking SAPI (php-fpm, Apache prefork) this happens in the master, so the result is shared with all workers via copy-on-write — paid once for the whole pool. The compile hook then becomes a lock-free lookup. See [Memory model](#memory-model-php-fpm). |
| `phpatcher.cache` | `1` | Lazily cache patched output for files that were *not* precomputed (e.g. created after startup, or when `precompute=0`). This cache is **per process**, so it is not shared across an fpm pool. |

To minimise resident memory at the cost of CPU on cold compiles, set both
`phpatcher.precompute=0` and `phpatcher.cache=0`: each file is then patched on the
fly every time the engine compiles it, and only the resulting opcodes are cached
(by OPcache, in shared memory).

## Patch formats

phpatcher auto-detects the patch file format:

1. **Unified diff** (`git diff` / `diff -u`) — the default. Easy to read, but it
   embeds the original source: both context lines and the removed (`-`) lines.

2. **ed-script bundle** (`diff -e`) — describes changes purely by line numbers
   and the new text, so it **never quotes the original/removed lines**. Use this
   when the patch must not leak the source it modifies. The format is a thin
   wrapper that adds per-file headers (a plain `diff -e` has neither a filename
   nor multi-file support):

   ```
   # phpatcher-ed v1
   # file: src/Foo.php
   12c
   $new = 'replacement';
   .
   20,21d
   # file: src/Bar.php
   3a
   inserted line
   .
   ```

   - The bundle is selected automatically when the file begins with the
     `# phpatcher-ed` magic line.
   - `# file: <path>` starts each file section (path resolved against
     `phpatcher.base_dir`).
   - Supported ed commands: `Na` (append after line N), `Ni` (insert before N),
     `M,Nc` / `Nc` (change), `M,Nd` / `Nd` (delete). Each `a/c/i` input block
     ends with a lone `.` line. Commands are applied in order, so emit them in
     descending line order exactly as `diff -e` does.
   - Limitation: a replacement line consisting solely of `.` cannot be
     represented (an ed-script convention). PHP source effectively never has one.

To produce an ed-script bundle from work-in-progress, `tools/changes-to-patch.sh`
converts the uncommitted changes of a git repository into one (it diffs each
modified file's `HEAD` version against the working tree with `diff -e`):

```bash
# inside your repo, with uncommitted edits in the working tree:
tools/changes-to-patch.sh -o changes.patch
# then point phpatcher at it (base_dir = repo root) and revert the working tree
```

## OPcache & JIT

phpatcher is designed to live alongside **OPcache** and the **JIT**, and is
tested against both (see `tests/008-opcache-jit.phpt`).

- **OPcache.** phpatcher installs its `zend_compile_file` hook in the same chain
  OPcache uses, and injects the patched source *before* the real compiler runs.
  OPcache therefore compiles and caches the **patched** opcodes. Once warm,
  there is no per-request patching cost — OPcache serves the cached, patched
  script straight from shared memory. Works with SHM and `opcache.file_cache`,
  and with `opcache.preload` (preloaded files are patched too).
- **JIT.** The JIT operates on the compiled opcodes, i.e. *downstream* of the
  source injection, so it simply JIT-compiles the patched code. phpatcher does
  **not** override `zend_execute_ex` / `zend_execute_internal`, so it never
  forces the JIT off (only extensions that hook execution, such as coverage
  tools like pcov/Xdebug, do that — independently of phpatcher).

> **Operational note:** OPcache keys its cache by the *source file* (path +
> mtime/size), not by the patch. If you change `phpatcher.patch_file` while the
> source files on disk stay the same, OPcache will keep serving the previously
> cached version. Reset OPcache (`opcache_reset()`, or restart PHP-FPM) after
> changing the patch. Editing a source file invalidates its entry normally and
> it is re-patched on the next compile. The same applies to `opcache.file_cache`:
> clear the on-disk cache directory when you change or first enable the patch, or
> it may load pre-patch opcodes from a previous run.

### Load order

The hook is a shared function pointer; whoever assigns it **last** runs **first**.
For best results phpatcher should sit *under* OPcache, so OPcache serves cache
hits straight from shared memory and only calls phpatcher on a miss (the first
compile of each file). This is the usual outcome, but if you load extensions in
an unusual order and patching does not take effect, ensure OPcache is loaded such
that it initialises after phpatcher (a `zend_extension=opcache` line is processed
after `extension=phpatcher.so`). phpatcher always chains to the previous handler,
so it is correct either way; the order only affects how often its (cheap) hook
runs.

## Memory model (php-fpm)

A common worry with "patch the whole app in memory" is that 100 fpm workers would
each hold their own copy. They do not:

- `MINIT` — where precomputation happens — runs **once in the fpm master**, before
  any worker is forked. Workers are `fork()`ed from the master, so the precomputed
  map is shared with every worker through **copy-on-write**. It is never written
  after startup, so the pages stay shared for the process lifetime.
- The footprint is therefore **`1 ×` the total size of the patched files**, for
  the whole pool — not `N ×` — and it covers only the files named in the patch,
  not the whole codebase.
- Per request a worker copies one patched file into a transient buffer handed to
  the compiler, freed right after; the compiled opcodes live once in OPcache's
  shared memory.

> Per-process `RSS` counts shared copy-on-write pages in *every* worker, so
> summing `RSS` across the pool massively over-counts. Measure real usage with
> `PSS`/`USS` (e.g. `smem`, or `/proc/<pid>/smaps_rollup`).

Counter-intuitively, the thing that *would* multiply memory per worker is a lazy
per-process cache filled inside workers — which is exactly why precomputation in
the master is the default. If your patch set is huge and you would rather trade
CPU for memory, see the `precompute`/`cache` knobs above.

## Behaviour & guarantees

- **Fail-safe.** If a patch does not apply (e.g. the source has changed and the
  context no longer matches), phpatcher compiles the *original* file and (in
  strict mode) emits a warning. It never produces a broken hybrid.
- **Only listed files are touched.** Any file not present in the patch index is
  compiled completely untouched (zero overhead beyond a hash-map lookup).
- **Thread-safe.** The patch index is immutable after startup; the patched-output
  cache is mutex-guarded for ZTS builds.

## Userland helpers & monitoring

```php
phpatcher_enabled(): bool     // is the hook active?
phpatcher_get_files(): array  // canonical paths currently indexed
phpatcher_stats(): array      // runtime counters (see below)
```

`phpatcher_stats()` returns:

```php
[
    'active'            => true,   // hook installed
    'strict'            => true,
    'precompute'        => true,
    'cache'             => true,
    'indexed_files'     => 12,     // files named in the patch
    'precomputed_files' => 12,     // patched at startup (shared via copy-on-write)
    'precomputed_bytes' => 348160, // shared footprint, paid once per fpm pool
    'cached_files'      => 0,      // lazily patched in this worker (per process)
    'cached_bytes'      => 0,
]
```

Use `precomputed_bytes` to size the shared cost and `cached_bytes` to watch
per-worker growth. `phpinfo()` / `php --ri phpatcher` also report the version,
active state, indexed file count and the precomputed file count and byte size.

## Security considerations

- `phpatcher.patch_file`, `phpatcher.base_dir` and all toggles are
  `PHP_INI_SYSTEM`: they can only be set by the server administrator in
  `php.ini`, never by `ini_set()` or per-directory `.user.ini`. Userland code
  cannot redirect what gets patched.
- The patch file is trusted input — it can replace the body of any PHP file the
  process can read. Protect it with the same care as the code it modifies
  (restrictive ownership/permissions, kept out of web-writable locations).
- phpatcher only ever **reads** source files and writes the patched bytes into
  memory; it never modifies anything on disk.

## Limitations

- Targets plain filesystem includes. Sources loaded through stream wrappers
  (e.g. `phar://`) are not patched.
- The patch index covers files that exist on disk. Patches that create brand new
  files (`--- /dev/null`) are not served, since such a file cannot be `include`d
  from disk in the first place.
- Patch application is strict (no fuzz): the hunk context must match the original
  file exactly, just like `git apply` without fuzzing.

## Project layout

```
patch.hpp / patch.cpp      Pure C++ core: unified-diff parser, applier, index.
                           No PHP dependency — independently unit-testable.
phpatcher.cpp              PHP glue: INI, lifecycle, the zend_compile_file hook.
php_phpatcher.h            Module header.
config.m4                  Build configuration (C++17).
tools/changes-to-patch.sh  Turn uncommitted git changes into an ed-script bundle.
tests/*.phpt               PHP integration tests (run via `make test`).
tests/fixtures/            Fixtures for the integration tests.
tests/unit/                Standalone C++ unit tests for the core.
```

## Running the tests

```bash
# PHP integration tests
make test

# C++ unit tests for the core (no PHP needed)
./tests/unit/run.sh          # or: make -C tests/unit
make -C tests/unit sanitize  # under AddressSanitizer + UndefinedBehaviorSanitizer
```

## License

[MIT](LICENSE) © Evgeny Stepanischev
