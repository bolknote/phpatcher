--TEST--
phpatcher: files not present in the patch compile unmodified
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
require __DIR__ . '/fixtures/passthrough_src.php';
echo passthrough_value(), "\n";
?>
--EXPECT--
CLEAN
