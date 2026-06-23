--TEST--
phpatcher: on_error=fail refuses to run an unpatchable __halt_compiler() file
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/halt.patch
phpatcher.base_dir={PWD}
phpatcher.on_error=fail
phpatcher.strict=0
--FILE--
<?php
// __halt_compiler() cannot be patched safely. Under fail-closed that still
// counts as "the patch did not take effect", so the file becomes a throwing
// stub rather than silently leaking the unpatched original.
try {
    require __DIR__ . '/fixtures/halt_src.php';
    echo "NO-THROW\n";
} catch (\RuntimeException $e) {
    echo (strpos($e->getMessage(), '__halt_compiler') !== false)
        ? "THREW-HALT\n" : "WRONG-MESSAGE\n";
}
?>
--EXPECT--
THREW-HALT
