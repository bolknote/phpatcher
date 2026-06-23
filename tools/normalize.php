<?php
declare(strict_types=1);
/*
 * phpatcher normalizer coprocess.
 *
 * A long-lived helper that phpatcher-index spawns once and feeds file contents
 * to over a pipe. For every physical line it returns a normalized "match key":
 *
 *   - Ordinary code lines: inter-token whitespace is collapsed, so
 *     `while(true)` and `while   (  true )` produce the same key. The PHP
 *     tokenizer is used (not a regex) so we never touch whitespace that lives
 *     *inside* a token (string/heredoc/nowdoc bodies, comments).
 *
 *   - Lines covered by a multi-line token (heredoc/nowdoc, multi-line strings,
 *     multi-line comments, multi-line inline HTML) are marked "indivisible":
 *     their key is the raw line bytes (CR stripped) so they can only match
 *     byte-for-byte. We never normalize across line boundaries.
 *
 * Wire protocol (binary, little-endian u32 via pack('V')):
 *
 *   request:  u32 length, <length bytes of file content>
 *   response: u32 nlines, then per physical line:
 *                 u8  flag   (0 = normal, 1 = indivisible)
 *                 u32 klen
 *                 <klen bytes of key>
 *
 * EOF on stdin -> clean shutdown. All diagnostics are silenced so they can
 * never corrupt the binary stream on stdout.
 */

error_reporting(0);
ini_set('display_errors', '-1');
ini_set('log_errors', '0');

/* Read exactly $n bytes from a blocking stdin. On a blocking stream
 * stream_get_contents() loops internally until it has $n bytes or hits EOF;
 * a short read therefore means the parent closed the pipe (clean shutdown). */
function read_exact(int $n): ?string {
    if ($n === 0) {
        return '';
    }
    $buf = stream_get_contents(STDIN, $n);
    if ($buf === false || strlen($buf) !== $n) {
        return null;
    }
    return $buf;
}

function normalize(string $src): string {
    if ($src === '') {
        return pack('V', 0);
    }

    $nlines = substr_count($src, "\n");
    if (!str_ends_with($src, "\n")) {
        $nlines++;  /* trailing partial line with no newline */
    }

    $keys = array_fill(0, $nlines, '');
    $indiv = [];   /* sparse: only multi-line (indivisible) line indices are set */

    $tokens = token_get_all($src);
    $cur = 0;

    foreach ($tokens as $tok) {
        if (is_array($tok)) {
            $id = $tok[0];
            $text = $tok[1];
            if ($id === T_WHITESPACE) {
                $cur += substr_count($text, "\n");
                continue;
            }
        } else {
            $text = $tok;
        }

        /* Fast path: the token lives entirely on one line (the overwhelming
         * majority — operators, identifiers, single-line strings/comments).
         * Append it to the current line's key without splitting. */
        if (strpos($text, "\n") === false) {
            if ($cur < $nlines) {
                if ($keys[$cur] !== '') {
                    $keys[$cur] .= ' ';
                }
                $keys[$cur] .= $text;
            }
            continue;
        }

        /* Slow path: a token that spans newlines. A token like "<?php\n" or
         * "// x\n" carries a trailing newline but only has content on one line,
         * so it is NOT multi-line; only tokens with content on 2+ lines (heredoc
         * / nowdoc bodies, multi-line strings/comments) are "indivisible". */
        $segs = explode("\n", $text);
        $nseg = count($segs);

        $first = -1;
        $nonempty = 0;
        foreach ($segs as $j => $s) {
            if ($s !== '') {
                $nonempty++;
                if ($first < 0) {
                    $first = $j;
                }
            }
        }

        if ($nonempty >= 2) {
            foreach ($segs as $j => $s) {
                $l = $cur + $j;
                if ($s !== '' && $l < $nlines) {
                    $indiv[$l] = true;
                }
            }
        } elseif ($first >= 0) {
            $l = $cur + $first;
            if ($l < $nlines) {
                if ($keys[$l] !== '') {
                    $keys[$l] .= ' ';
                }
                $keys[$l] .= $segs[$first];
            }
        }

        $cur += $nseg - 1;
    }

    /* Common case: no indivisible lines, so we never need the raw line bytes and
     * can emit straight from $keys — skipping the explode() of the whole file. */
    if (!$indiv) {
        $body = '';
        foreach ($keys as $k) {
            $body .= "\x00" . pack('V', strlen($k)) . $k;
        }
        return pack('V', $nlines) . $body;
    }

    /* Indivisible lines exist (heredoc/nowdoc/multi-line string or comment):
     * their key is the raw line bytes (CR stripped), so materialize the lines. */
    $raw = explode("\n", $src);
    $body = '';
    for ($i = 0; $i < $nlines; $i++) {
        if (isset($indiv[$i])) {
            $flag = "\x01";
            $k = rtrim($raw[$i], "\r");
        } else {
            $flag = "\x00";
            $k = $keys[$i];
        }
        $body .= $flag . pack('V', strlen($k)) . $k;
    }

    return pack('V', $nlines) . $body;
}

for (;;) {
    $hdr = read_exact(4);
    if ($hdr === null) {
        break;
    }
    $len = unpack('V', $hdr)[1];
    $src = read_exact($len);
    if ($src === null) {
        break;
    }
    fwrite(STDOUT, normalize($src));
}
fflush(STDOUT);
