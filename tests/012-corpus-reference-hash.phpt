--TEST--
phpatcher: phpatcher-ed corpus reference (r) with hash (h:) guard is resolved
--SKIPIF--
<?php if (!extension_loaded('phpatcher')) die('skip phpatcher not available'); ?>
--INI--
phpatcher.patch_file={PWD}/fixtures/ref_hash.patch
phpatcher.base_dir={PWD}
--FILE--
<?php
// Same as 011, but the reference is guarded by a content hash (h:) instead of
// the byte length (s:). The opt-in "paranoid" path must resolve identically.
require __DIR__ . '/fixtures/ref_src.php';
echo function_exists('donor_helper') ? donor_helper() : "no-helper", "\n";

$patch = file_get_contents(__DIR__ . '/fixtures/ref_hash.patch');
echo (strpos($patch, 'FROM-DONOR') === false) ? "patch-dedup\n" : "patch-leaks\n";
echo (strpos($patch, ' # h:') !== false) ? "hash-guard\n" : "no-hash-guard\n";
?>
--EXPECT--
FROM-DONOR
patch-dedup
hash-guard
