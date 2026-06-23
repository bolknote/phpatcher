--TEST--
phpatcher: files using __halt_compiler() are compiled untouched (fail-open)
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/halt.patch
phpatcher.base_dir={PWD}
phpatcher.strict=0
--FILE--
<?php
// Patching a file with __halt_compiler() would shift the byte offset the engine
// records for __COMPILER_HALT_OFFSET__, so phpatcher refuses to patch it and
// compiles the original. With the default on_error=original that means the
// unpatched function runs and the skip is counted.
require __DIR__ . '/fixtures/halt_src.php';
echo halt_value(), "\n";

$disk = file_get_contents(__DIR__ . '/fixtures/halt_src.php');
echo (strpos($disk, 'ORIG') !== false) ? "disk-unchanged\n" : "disk-MODIFIED\n";

$s = phpatcher_stats();
echo "skipped_halt_positive=" . var_export($s['skipped_halt_compiler'] > 0, true) . "\n";
?>
--EXPECT--
ORIG
disk-unchanged
skipped_halt_positive=true
