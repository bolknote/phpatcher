--TEST--
phpatcher: a non-applying patch warns (strict) and falls back to the original
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/mismatch.patch
phpatcher.base_dir={PWD}
phpatcher.strict=1
--FILE--
<?php
set_error_handler(function ($no, $str) {
    echo "WARN\n";
    return true;
});
require __DIR__ . '/fixtures/mismatch_src.php';
restore_error_handler();
echo mismatch_value(), "\n";
?>
--EXPECT--
WARN
1
