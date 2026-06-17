--TEST--
phpatcher: with precompute off the patch still applies via the lazy per-process cache
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
phpatcher.precompute=0
phpatcher.cache=1
--FILE--
<?php
$before = phpatcher_stats();
echo "precompute=" . var_export($before['precompute'], true) . "\n";
echo "precomputed_before=" . $before['precomputed_files'] . "\n";

require __DIR__ . '/fixtures/basic_src.php';
echo basic_value(), "\n";

$after = phpatcher_stats();
echo "cached_after=" . $after['cached_files'] . "\n";
echo "cached_bytes_positive=" . var_export($after['cached_bytes'] > 0, true) . "\n";
?>
--EXPECT--
precompute=false
precomputed_before=0
PATCHED
cached_after=1
cached_bytes_positive=true
