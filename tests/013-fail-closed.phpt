--TEST--
phpatcher: on_error=fail replaces a non-applying file with a throwing stub
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/failclosed.patch
phpatcher.base_dir={PWD}
phpatcher.on_error=fail
phpatcher.strict=0
--FILE--
<?php
// The patch addresses line 99 of a 2-line file, so it can never apply. Under
// fail-closed the engine must compile a throwing stub instead of the original,
// so the unpatched (e.g. still-vulnerable) code never runs.
try {
    require __DIR__ . '/fixtures/failclosed_src.php';
    echo "NO-THROW\n";
} catch (\RuntimeException $e) {
    echo (strpos($e->getMessage(), 'refusing to run unpatched code') !== false)
        ? "THREW\n" : "WRONG-MESSAGE\n";
}

$s = phpatcher_stats();
echo "on_error=" . $s['on_error'] . "\n";
echo "apply_failures_positive=" . var_export($s['apply_failures'] > 0, true) . "\n";
?>
--EXPECT--
THREW
on_error=fail
apply_failures_positive=true
