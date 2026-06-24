--TEST--
phpatcher: a "# newfile:" section is created in memory and is require-able
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/newfile.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
$f = __DIR__ . '/fixtures/virtual_helper.php';
// The file does not exist on disk: phpatcher materializes it at compile time.
var_dump(file_exists($f));
echo in_array($f, phpatcher_get_files(), true) ? "indexed\n" : "not-indexed\n";
require $f;
echo helper_value(), "\n";
?>
--EXPECT--
bool(false)
indexed
FROM-MEMORY
