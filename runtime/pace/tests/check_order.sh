#!/bin/sh
# SPDX-License-Identifier: MIT
# Reads the arm64 assembly of tests/order_probe.c on standard input and checks where the barriers of vsync_lock.h's seqlock are:
#   writer (probe_write, probe_guest): the odd sequence is stored, then `dmb ish`, then the first field; the even sequence goes
#     out with a store-release (stlr) after the last field
#   reader (probe_read): the first sequence load is a load-acquire (ldar), the last field is loaded, then `dmb ishld`, then the
#     sequence is loaded again
# Exit 0 and "order ok" when all hold.
awk '
function fail(m) { print "check_order: " m; bad = 1 }
/^probe_[a-z]+:/ { fn = $1; sub(":", "", fn); n = 0; seen_odd = 0; seen_dmb = 0; first_field = 0; last_field = 0; next }
/^[ \t]*\.Lfunc_end/ { fn = "" }
fn == "" { next }
{
  n++
  line = $0
  if (fn == "probe_write" || fn == "probe_guest") {
    if (line ~ /str[ \t]+x[0-9]+, \[x0, #8\]/ && !seen_odd) { seen_odd = n; odd_line = n }
    if (line ~ /dmb[ \t]+ish$/ && !seen_dmb) { seen_dmb = n }
    if (line ~ /str[ \t]+x[0-9]+, \[x0, #(16|24|32|40)\]/) { if (!first_field) first_field = n; last_field = n }
    if (line ~ /stlr[ \t]+x[0-9]+, \[x[0-9]+\]/) { stlr_line = n; if (last_field && n > last_field) end_ok[fn] = 1 }
    if (line ~ /ret/) {
      if (!(seen_odd && seen_dmb && first_field && seen_odd < seen_dmb && seen_dmb < first_field)) fail(fn ": odd sequence, dmb ish, fields are not in that order")
      if (!end_ok[fn]) fail(fn ": the even sequence is not stored with release after the last field")
    }
  } else if (fn == "probe_read") {
    if (line ~ /ldar/) acq = 1
    if (line ~ /ldr[ \t]+x[0-9]+, \[x0, #40\]/) { field = n; dmb = 0 }
    if (line ~ /dmb[ \t]+ishld/ && field) dmb = n
    if (line ~ /ldr[ \t]+x[0-9]+, \[x0, #8\]/) { if (!(field && dmb && field < dmb && dmb < n)) fail("probe_read: the sequence is loaded again without `dmb ishld` after the fields"); else reread = 1 }
  }
}
END {
  if (!acq) fail("probe_read: no load-acquire of the sequence")
  if (!reread) fail("probe_read: no second sequence load")
  if (!bad) print "order ok"
  exit bad
}'
