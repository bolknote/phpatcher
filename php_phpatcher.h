/*
 * phpatcher - transparently apply phpatcher-ed patches to PHP source files in memory
 * at compile time, without ever modifying the files on disk.
 */
#ifndef PHP_PHPATCHER_H
#define PHP_PHPATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "php.h"

#define PHP_PHPATCHER_VERSION "1.0.0"

extern zend_module_entry phpatcher_module_entry;
#define phpext_phpatcher_ptr &phpatcher_module_entry

/* PHP 8.5 passes -DZEND_COMPILE_DL_EXT on the command line for dynamically
 * loaded extensions, but does not force-include the generated config.h (where
 * COMPILE_DL_PHPATCHER lives). Accept either so get_module() is emitted. */
#if !defined(COMPILE_DL_PHPATCHER) && defined(ZEND_COMPILE_DL_EXT)
#define COMPILE_DL_PHPATCHER 1
#endif

#if defined(ZTS) && defined(COMPILE_DL_PHPATCHER)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#ifdef __cplusplus
}
#endif

#endif /* PHP_PHPATCHER_H */
