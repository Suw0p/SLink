# Bundled libvterm

- Version: 0.3.3
- Upstream archive: `https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz`
- Archive SHA-256: `09156F43DD2128BD347CBEEBE50D9A571D32C64E0CF18D211197946AFF7226E0`
- License: MIT, see `LICENSE`

Only the library sources required by SLink are bundled. The public headers remain in
`3rd/include` so existing consumers use one ABI definition.

## SLink patch

`src/screen.c` stops the reflow scan at row zero instead of decrementing to `-1` when
the first visible row is itself a continuation. The subsequent code uses that row as
an array index, so the original boundary could cause the access violation observed in
terminal resize crash dumps. The inclusive cursor-range comparison is synchronized
with the corrected upstream implementation as well.

SLink additionally disables historical-line reflow in its adapter. The source-level
fix remains covered by a direct reflow regression test as defense in depth.

For memory-safety verification, configure with
`-DSLINK_ENABLE_ADDRESS_SANITIZER=ON -DSLINK_BUILD_TESTS=ON` and build a
`RelWithDebInfo` test target. The terminal resize regression suite is compatible with
MSVC AddressSanitizer.
