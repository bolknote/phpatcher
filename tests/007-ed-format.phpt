--TEST--
phpatcher: ed-script bundle (diff -e) patches are applied
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/ed.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
require __DIR__ . '/fixtures/ed_src.php';
echo ed_value(), "\n";
// The ed-script patch must not contain the original line at all.
$patch = file_get_contents(__DIR__ . '/fixtures/ed.patch');
echo (strpos($patch, 'ORIG') === false) ? "patch-clean\n" : "patch-leaks\n";
// On-disk source is untouched.
$disk = file_get_contents(__DIR__ . '/fixtures/ed_src.php');
echo (strpos($disk, 'ORIG') !== false) ? "disk-unchanged\n" : "disk-MODIFIED\n";
?>
--EXPECT--
PATCHED
patch-clean
disk-unchanged
