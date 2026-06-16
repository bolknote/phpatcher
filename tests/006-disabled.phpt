--TEST--
phpatcher: phpatcher.enabled=0 disables all patching
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.enabled=0
phpatcher.patch_file={PWD}/fixtures/basic.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
var_dump(phpatcher_enabled());
require __DIR__ . '/fixtures/basic_src.php';
echo basic_value(), "\n";
?>
--EXPECT--
bool(false)
ORIG
