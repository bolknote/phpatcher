--TEST--
phpatcher: require applies the patch in memory, file on disk is untouched
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
require __DIR__ . '/fixtures/basic_src.php';
echo basic_value(), "\n";
$disk = file_get_contents(__DIR__ . '/fixtures/basic_src.php');
echo (strpos($disk, 'ORIG') !== false && strpos($disk, 'PATCHED') === false)
    ? "disk-unchanged\n" : "disk-MODIFIED\n";
?>
--EXPECT--
PATCHED
disk-unchanged
