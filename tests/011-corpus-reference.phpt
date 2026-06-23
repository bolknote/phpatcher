--TEST--
phpatcher: phpatcher-ed corpus reference (r) is resolved and length-verified
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/ref.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
// The patched ref_src.php gains donor_helper() whose body is *not* quoted in
// the patch but pulled in by reference from ref_donor.php.
require __DIR__ . '/fixtures/ref_src.php';
echo function_exists('donor_helper') ? donor_helper() : "no-helper", "\n";

// The patch must not contain the referenced body verbatim (that is the point).
$patch = file_get_contents(__DIR__ . '/fixtures/ref.patch');
echo (strpos($patch, 'FROM-DONOR') === false) ? "patch-dedup\n" : "patch-leaks\n";

// On-disk target source is untouched.
$disk = file_get_contents(__DIR__ . '/fixtures/ref_src.php');
echo (strpos($disk, 'donor_helper') === false) ? "disk-unchanged\n" : "disk-MODIFIED\n";
?>
--EXPECT--
FROM-DONOR
patch-dedup
disk-unchanged
