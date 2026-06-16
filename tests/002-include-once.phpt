--TEST--
phpatcher: include_once is patched and still deduplicated
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
$a = include_once __DIR__ . '/fixtures/basic_src.php';
$b = include_once __DIR__ . '/fixtures/basic_src.php';
var_dump($b); // second include_once returns true (already included)
echo basic_value(), "\n";
?>
--EXPECT--
bool(true)
PATCHED
