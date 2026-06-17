/*
 * phpatcher - PHP extension entry point.
 *
 * Hooks the engine's `zend_compile_file` function pointer (the same supported
 * extension point used by OPcache/Xdebug) to transparently apply a unified
 * diff to PHP source files *in memory* right before they are compiled. The
 * files on disk are never modified.
 */

#include "patch.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

extern "C" {
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_API.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_stream.h"
#include "Zend/zend_virtual_cwd.h"
}

#include "php_phpatcher.h"

/* ------------------------------------------------------------------------- */
/* Module globals (INI-backed configuration).                                */
/* ------------------------------------------------------------------------- */

ZEND_BEGIN_MODULE_GLOBALS(phpatcher)
    char *patch_file;
    char *base_dir;
    bool enabled;
    bool strict;
    bool precompute;
    bool cache;
ZEND_END_MODULE_GLOBALS(phpatcher)

ZEND_DECLARE_MODULE_GLOBALS(phpatcher)

#define PHPATCHER_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(phpatcher, v)

#if defined(ZTS) && defined(COMPILE_DL_PHPATCHER)
ZEND_TSRMLS_CACHE_DEFINE()
#endif

/* ------------------------------------------------------------------------- */
/* Process-global state (immutable / synchronized after MINIT).              */
/* ------------------------------------------------------------------------- */

/* The parsed patch index is built once in MINIT and is read-only afterwards,
 * so it is safe to share across threads without locking. */
static phpatcher::PatchSet *g_patchset = nullptr;

/* Patched contents precomputed once in MINIT, keyed by canonical path. Built
 * before any request runs and never mutated afterwards, so the compile hook can
 * read it lock-free (no mutex, no file I/O, no patch application on the hot
 * path). Under a forking SAPI (php-fpm, prefork) MINIT runs in the master, so
 * this map is shared with every worker via copy-on-write: its cost is paid once
 * for the whole pool, not per worker. Files that could not be read/applied at
 * startup are simply absent and fall back to the lazy cache below. */
static std::unordered_map<std::string, std::string> *g_precomputed = nullptr;

/* Lazy, per-process cache of patched contents for files not precomputed (e.g.
 * created after startup, or when precompute is disabled). Unlike g_precomputed
 * this is populated inside workers, so it is NOT shared across a php-fpm pool.
 * Guarded by a mutex for ZTS builds; on NTS there is no contention. */
static std::unordered_map<std::string, std::string> *g_cache = nullptr;
static std::mutex g_cache_mutex;

/* Previous compile_file handler in the chain (e.g. OPcache). */
static zend_op_array *(*phpatcher_orig_compile_file)(zend_file_handle *, int) = nullptr;

/* ------------------------------------------------------------------------- */
/* INI                                                                        */
/* ------------------------------------------------------------------------- */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("phpatcher.patch_file", "", PHP_INI_SYSTEM, OnUpdateString,
                      patch_file, zend_phpatcher_globals, phpatcher_globals)
    STD_PHP_INI_ENTRY("phpatcher.base_dir", "", PHP_INI_SYSTEM, OnUpdateString,
                      base_dir, zend_phpatcher_globals, phpatcher_globals)
    STD_PHP_INI_BOOLEAN("phpatcher.enabled", "1", PHP_INI_SYSTEM, OnUpdateBool,
                        enabled, zend_phpatcher_globals, phpatcher_globals)
    STD_PHP_INI_BOOLEAN("phpatcher.strict", "1", PHP_INI_SYSTEM, OnUpdateBool,
                        strict, zend_phpatcher_globals, phpatcher_globals)
    STD_PHP_INI_BOOLEAN("phpatcher.precompute", "1", PHP_INI_SYSTEM, OnUpdateBool,
                        precompute, zend_phpatcher_globals, phpatcher_globals)
    STD_PHP_INI_BOOLEAN("phpatcher.cache", "1", PHP_INI_SYSTEM, OnUpdateBool,
                        cache, zend_phpatcher_globals, phpatcher_globals)
PHP_INI_END()

static void php_phpatcher_globals_ctor(zend_phpatcher_globals *g) {
    g->patch_file = nullptr;
    g->base_dir = nullptr;
    g->enabled = true;
    g->strict = true;
    g->precompute = true;
    g->cache = true;
}

/* ------------------------------------------------------------------------- */
/* The compile hook.                                                          */
/* ------------------------------------------------------------------------- */

/* The basename of the file the engine is about to compile, as a view into the
 * handle's path (no copy). */
static std::string_view phpatcher_basename(zend_file_handle *fh) {
    const char *p = nullptr;
    size_t len = 0;
    if (fh->opened_path) {
        p = ZSTR_VAL(fh->opened_path);
        len = ZSTR_LEN(fh->opened_path);
    } else if (fh->filename) {
        p = ZSTR_VAL(fh->filename);
        len = ZSTR_LEN(fh->filename);
    }
    if (p == nullptr) {
        return {};
    }
    const std::string_view path(p, len);
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/* Resolve the file handle to a canonical absolute path string. */
static bool phpatcher_canonical_path(zend_file_handle *fh, std::string &out) {
    zend_string *resolved = nullptr;
    if (fh->opened_path) {
        resolved = zend_string_copy(fh->opened_path);
    } else if (fh->filename) {
        resolved = zend_resolve_path(fh->filename);
    }
    if (!resolved) {
        return false;
    }
    out = phpatcher::PatchSet::canonicalize(ZSTR_VAL(resolved), std::string());
    zend_string_release(resolved);
    return true;
}

/* Find the patch for the file about to be compiled, or nullptr. On a hit, `path`
 * is set to the matching canonical key. A syscall-free basename pre-filter runs
 * first, so unpatched files (the common case) never reach realpath(). */
static const phpatcher::FilePatch *phpatcher_resolve(zend_file_handle *fh, std::string &path) {
    const std::string_view base = phpatcher_basename(fh);
    if (base.empty() || !g_patchset->has_basename(std::string(base))) {
        return nullptr;
    }

    /* opened_path, when set, is already the engine's resolved canonical path:
     * try it directly before paying for a realpath(). */
    if (fh->opened_path) {
        path.assign(ZSTR_VAL(fh->opened_path), ZSTR_LEN(fh->opened_path));
        if (const phpatcher::FilePatch *fp = g_patchset->find(path)) {
            return fp;
        }
    }

    /* Fallback that resolves relative paths and symlinks. Preserves correctness
     * when opened_path differs from the canonical key. */
    std::string canon;
    if (phpatcher_canonical_path(fh, canon)) {
        if (const phpatcher::FilePatch *fp = g_patchset->find(canon)) {
            path = std::move(canon);
            return fp;
        }
    }
    return nullptr;
}

/* Hand the patched source to the engine via the file handle's in-memory buffer.
 * zend_stream_fixup short-circuits on a pre-set buf, so the file is never
 * re-read and the engine compiles exactly our content. The buffer must be
 * emalloc'd with ZEND_MMAP_AHEAD trailing zero bytes (the lexer reads ahead) and
 * is efree'd by zend_destroy_file_handle. */
static void phpatcher_inject_source(zend_file_handle *fh, const std::string &content,
                                    const std::string &path) {
    const size_t len = content.size();
    char *buf = static_cast<char *>(emalloc(len + ZEND_MMAP_AHEAD));
    memcpy(buf, content.data(), len);
    memset(buf + len, 0, ZEND_MMAP_AHEAD);
    fh->buf = buf;
    fh->len = len;

    /* Make __FILE__ and include_once bookkeeping use the real path. */
    if (!fh->opened_path && fh->filename) {
        fh->opened_path = zend_string_init(path.c_str(), path.size(), 0);
    }
}

/* Produce the patched content for `path`, using the cache when enabled.
 * Returns true and fills `content` on success. */
static bool phpatcher_get_patched(const std::string &path, const phpatcher::FilePatch &fp,
                                  std::string &content) {
    /* Lock-free fast path: content prepared at startup. */
    if (g_precomputed) {
        auto it = g_precomputed->find(path);
        if (it != g_precomputed->end()) {
            content = it->second;
            return true;
        }
    }

    const bool use_cache = PHPATCHER_G(cache);

    if (use_cache && g_cache) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache->find(path);
        if (it != g_cache->end()) {
            content = it->second;
            return true;
        }
    }

    std::string original;
    if (!phpatcher::read_file(path, original)) {
        if (PHPATCHER_G(strict)) {
            php_error_docref(nullptr, E_WARNING,
                             "phpatcher: cannot read original file '%s'", path.c_str());
        }
        return false;
    }

    std::string error;
    if (!phpatcher::PatchSet::apply(fp, original, content, error)) {
        if (PHPATCHER_G(strict)) {
            php_error_docref(nullptr, E_WARNING,
                             "phpatcher: patch did not apply to '%s': %s",
                             path.c_str(), error.c_str());
        }
        return false;
    }

    if (use_cache && g_cache) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        (*g_cache)[path] = content;
    }
    return true;
}

static zend_op_array *phpatcher_compile_file(zend_file_handle *file_handle, int type) {
    /* Eligible to patch only when we have an index, a path to look up, and the
     * source has not already been buffered by an earlier hook. */
    const bool eligible =
        g_patchset != nullptr && file_handle != nullptr && file_handle->buf == nullptr &&
        (file_handle->filename != nullptr || file_handle->opened_path != nullptr);

    if (eligible) {
        std::string path;
        if (const phpatcher::FilePatch *fp = phpatcher_resolve(file_handle, path)) {
            std::string patched;
            if (phpatcher_get_patched(path, *fp, patched)) {
                phpatcher_inject_source(file_handle, patched, path);
            }
            /* On failure we fall through and compile the original (fail-safe). */
        }
    }

    return phpatcher_orig_compile_file(file_handle, type);
}

/* ------------------------------------------------------------------------- */
/* Userland introspection helpers.                                            */
/* ------------------------------------------------------------------------- */

/* Total byte size of all patched contents held in a content map. */
static size_t phpatcher_map_bytes(const std::unordered_map<std::string, std::string> &m) {
    size_t total = 0;
    for (const auto &kv : m) {
        total += kv.second.size();
    }
    return total;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phpatcher_get_files, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(phpatcher_get_files) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    if (!g_patchset) {
        return;
    }
    for (const auto &kv : g_patchset->files()) {
        add_next_index_stringl(return_value, kv.first.c_str(), kv.first.size());
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phpatcher_enabled, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(phpatcher_enabled) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(g_patchset != nullptr);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phpatcher_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Runtime stats for monitoring. `precomputed_bytes` is the copy-on-write memory
 * shared across a php-fpm pool (paid once); `cached_bytes` is per-process. */
PHP_FUNCTION(phpatcher_stats) {
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);

    add_assoc_bool(return_value, "active", g_patchset != nullptr);
    add_assoc_bool(return_value, "strict", PHPATCHER_G(strict));
    add_assoc_bool(return_value, "precompute", PHPATCHER_G(precompute));
    add_assoc_bool(return_value, "cache", PHPATCHER_G(cache));
    add_assoc_long(return_value, "indexed_files",
                   g_patchset ? static_cast<zend_long>(g_patchset->file_count()) : 0);

    zend_long precomputed_files = 0, precomputed_bytes = 0;
    if (g_precomputed) {
        precomputed_files = static_cast<zend_long>(g_precomputed->size());
        precomputed_bytes = static_cast<zend_long>(phpatcher_map_bytes(*g_precomputed));
    }
    add_assoc_long(return_value, "precomputed_files", precomputed_files);
    add_assoc_long(return_value, "precomputed_bytes", precomputed_bytes);

    zend_long cached_files = 0, cached_bytes = 0;
    if (g_cache) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        cached_files = static_cast<zend_long>(g_cache->size());
        cached_bytes = static_cast<zend_long>(phpatcher_map_bytes(*g_cache));
    }
    add_assoc_long(return_value, "cached_files", cached_files);
    add_assoc_long(return_value, "cached_bytes", cached_bytes);
}

static const zend_function_entry phpatcher_functions[] = {
    PHP_FE(phpatcher_get_files, arginfo_phpatcher_get_files)
    PHP_FE(phpatcher_enabled, arginfo_phpatcher_enabled)
    PHP_FE(phpatcher_stats, arginfo_phpatcher_stats)
    PHP_FE_END
};

/* ------------------------------------------------------------------------- */
/* Module lifecycle.                                                          */
/* ------------------------------------------------------------------------- */

/* Resolve the base directory used to anchor the patch's relative paths: the
 * configured value, or the process CWD as a fallback. */
static std::string phpatcher_base_dir() {
    const char *cfg = PHPATCHER_G(base_dir);
    if (cfg != nullptr && cfg[0] != '\0') {
        return cfg;
    }
    char cwd[MAXPATHLEN];
    return VCWD_GETCWD(cwd, MAXPATHLEN) ? std::string(cwd) : std::string();
}

/* Populate g_precomputed with the patched content of every indexed file (see
 * the g_precomputed declaration). Best-effort: files that cannot be read or do
 * not apply now are handled lazily on first compile. */
static void phpatcher_precompute(const phpatcher::PatchSet &set) {
    g_precomputed = new (std::nothrow) std::unordered_map<std::string, std::string>();
    if (g_precomputed == nullptr) {
        return;
    }
    for (const auto &kv : set.files()) {
        std::string original, patched, error;
        if (phpatcher::read_file(kv.first, original) &&
            phpatcher::PatchSet::apply(kv.second, original, patched, error)) {
            (*g_precomputed)[kv.first] = std::move(patched);
        }
    }
}

PHP_MINIT_FUNCTION(phpatcher) {
    REGISTER_INI_ENTRIES();

    if (!PHPATCHER_G(enabled)) {
        return SUCCESS;
    }

    const char *patch_file = PHPATCHER_G(patch_file);
    if (patch_file == nullptr || patch_file[0] == '\0') {
        /* No patch configured: stay completely silent and install no hook.
         * This makes it safe to load the extension globally in php.ini. */
        return SUCCESS;
    }

    std::string diff_text;
    if (!phpatcher::read_file(patch_file, diff_text)) {
        php_error_docref(nullptr, E_WARNING,
                         "phpatcher: cannot read patch file '%s'", patch_file);
        return SUCCESS;
    }

    auto *set = new (std::nothrow) phpatcher::PatchSet();
    if (set == nullptr) {
        return SUCCESS;
    }

    std::string error;
    if (!set->parse(diff_text, phpatcher_base_dir(), error)) {
        php_error_docref(nullptr, E_WARNING,
                         "phpatcher: failed to parse patch '%s': %s",
                         patch_file, error.c_str());
        delete set;
        return SUCCESS;
    }

    if (set->file_count() == 0) {
        /* A valid but empty patch (no file sections) is a no-op: install no
         * hook and stay silent. */
        delete set;
        return SUCCESS;
    }

    g_patchset = set;
    g_cache = new (std::nothrow) std::unordered_map<std::string, std::string>();

    /* Precompute patched content so the compile hook is a lock-free lookup +
     * memcpy on the hot path, and (under a forking SAPI) so the cost is paid
     * once in the master and shared with workers via copy-on-write. */
    if (PHPATCHER_G(precompute)) {
        phpatcher_precompute(*g_patchset);
    }

    /* Install our hook at the head of the chain. */
    phpatcher_orig_compile_file = zend_compile_file;
    zend_compile_file = phpatcher_compile_file;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(phpatcher) {
    /* Unchain only if we are still the head: if another extension wrapped us
     * after MINIT, blindly restoring the pointer would drop its handler. (In
     * the usual reverse-shutdown order it has already unwrapped us by now.) */
    if (phpatcher_orig_compile_file != nullptr &&
        zend_compile_file == phpatcher_compile_file) {
        zend_compile_file = phpatcher_orig_compile_file;
        phpatcher_orig_compile_file = nullptr;
    }

    delete g_patchset;
    g_patchset = nullptr;
    delete g_precomputed;
    g_precomputed = nullptr;
    delete g_cache;
    g_cache = nullptr;

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(phpatcher) {
    php_info_print_table_start();
    php_info_print_table_header(2, "phpatcher support", "enabled");
    php_info_print_table_row(2, "Version", PHP_PHPATCHER_VERSION);
    php_info_print_table_row(2, "Active",
                             g_patchset != nullptr ? "yes" : "no");
    if (g_patchset != nullptr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", g_patchset->file_count());
        php_info_print_table_row(2, "Indexed files", buf);
        if (g_precomputed != nullptr) {
            snprintf(buf, sizeof(buf), "%zu", g_precomputed->size());
            php_info_print_table_row(2, "Precomputed files", buf);
            snprintf(buf, sizeof(buf), "%zu", phpatcher_map_bytes(*g_precomputed));
            php_info_print_table_row(2, "Precomputed bytes (shared)", buf);
        }
    }
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

PHP_GINIT_FUNCTION(phpatcher) {
#if defined(COMPILE_DL_PHPATCHER) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    php_phpatcher_globals_ctor(phpatcher_globals);
}

zend_module_entry phpatcher_module_entry = {
    STANDARD_MODULE_HEADER,
    "phpatcher",
    phpatcher_functions,
    PHP_MINIT(phpatcher),
    PHP_MSHUTDOWN(phpatcher),
    nullptr, /* RINIT */
    nullptr, /* RSHUTDOWN */
    PHP_MINFO(phpatcher),
    PHP_PHPATCHER_VERSION,
    PHP_MODULE_GLOBALS(phpatcher),
    PHP_GINIT(phpatcher),
    nullptr, /* GSHUTDOWN */
    nullptr, /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX};

#ifdef COMPILE_DL_PHPATCHER
ZEND_GET_MODULE(phpatcher)
#endif
