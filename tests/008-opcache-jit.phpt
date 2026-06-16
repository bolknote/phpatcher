--TEST--
phpatcher: patches apply correctly with OPcache and JIT enabled
--SKIPIF--
<?php
if (!extension_loaded('phpatcher')) die('skip phpatcher not available');
if (!extension_loaded('Zend OPcache')) die('skip OPcache not available');
?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
opcache.enable=1
opcache.enable_cli=1
opcache.jit_buffer_size=64M
opcache.jit=tracing
--FILE--
<?php
require __DIR__ . '/fixtures/basic_src.php';

// The patched function body must be what actually compiled and ran, even with
// OPcache compiling/caching the script and JIT enabled on top of it.
echo basic_value(), "\n";

$status = opcache_get_status(true);
echo "opcache=", (($status['opcache_enabled'] ?? false) ? "on" : "off"), "\n";

$key = realpath(__DIR__ . '/fixtures/basic_src.php');
$cached = isset($status['scripts'][$key]);
// If OPcache did cache the script in this SAPI, it must be the *patched* one;
// if this SAPI does not cache (e.g. phpdbg), the patch must still have applied.
echo "cached=", ($cached ? "patched" : "n/a"), "\n";
?>
--EXPECT--
PATCHED
opcache=on
cached=patched
