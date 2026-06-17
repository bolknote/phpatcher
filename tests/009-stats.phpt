--TEST--
phpatcher: phpatcher_stats() reports precompute as shared, cache as per-process
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
require __DIR__ . '/fixtures/basic_src.php';
echo basic_value(), "\n";

$s = phpatcher_stats();
echo "active="      . var_export($s['active'], true) . "\n";
echo "indexed="     . $s['indexed_files'] . "\n";
echo "precompute="  . var_export($s['precompute'], true) . "\n";
echo "precomputed=" . $s['precomputed_files'] . "\n";
echo "precomputed_bytes_positive=" . var_export($s['precomputed_bytes'] > 0, true) . "\n";
echo "cache="       . var_export($s['cache'], true) . "\n";
echo "cached="      . $s['cached_files'] . "\n";
?>
--EXPECT--
PATCHED
active=true
indexed=1
precompute=true
precomputed=1
precomputed_bytes_positive=true
cache=true
cached=0
