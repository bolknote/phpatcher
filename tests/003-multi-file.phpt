--TEST--
phpatcher: a single patch covering multiple files patches each one
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/multi.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
require __DIR__ . '/fixtures/multi_a.php';
require __DIR__ . '/fixtures/multi_b.php';
echo multi_a(), "\n";
echo multi_b(), "\n";
echo count(phpatcher_get_files()), "\n";
?>
--EXPECT--
A2
B2
2
