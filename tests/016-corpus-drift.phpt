--TEST--
phpatcher: a drifted corpus reference (guard mismatch) fails instead of guessing
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/drift.patch
phpatcher.base_dir={PWD}
phpatcher.strict=0
--FILE--
<?php
// The patch pulls donor_helper() in by reference, but its s: guard claims a
// byte length that does not match the on-disk donor. The resolver must reject
// the drifted reference; with the default on_error=original the target compiles
// untouched (no donor_helper), and the failure is observable via stats.
require __DIR__ . '/fixtures/drift_src.php';
echo function_exists('donor_helper') ? "LEAKED\n" : "no-helper\n";

$s = phpatcher_stats();
echo "apply_failures_positive=" . var_export($s['apply_failures'] > 0, true) . "\n";
echo (strpos($s['last_error'], 'has changed') !== false) ? "drift-detected\n" : "no-drift\n";
?>
--EXPECT--
no-helper
apply_failures_positive=true
drift-detected
