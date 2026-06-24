--TEST--
phpatcher: a "# newfile:" that cannot be produced throws under on_error=fail
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/newfile_failclosed.patch
phpatcher.base_dir={PWD}
phpatcher.on_error=fail
phpatcher.strict=0
--FILE--
<?php
// The created file references a corpus file that does not exist, so the patch
// cannot be produced. Under on_error=fail phpatcher compiles a throwing stub in
// place of the file instead of falling back to a non-existent original.
try {
    require __DIR__ . '/fixtures/virtual_broken.php';
    echo "NO-THROW\n";
} catch (\Throwable $e) {
    echo "THREW: ", ($e instanceof \RuntimeException ? "RuntimeException" : get_class($e)), "\n";
}
?>
--EXPECT--
THREW: RuntimeException
