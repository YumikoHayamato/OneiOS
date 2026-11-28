// SPDX-License-Identifier: MIT
// Writes the loader tests' generated kernel (tests/fixture.h: an MH_EXECUTE with a symbol table, no Apple bytes) to a
// file, for the checks that run the tools themselves (tests/test_crosvm_tool.sh). With a second file name also the
// generated identity tree (fx_identity, flat as Apple's decoded device tree is: the --identity-tree of xnuloader-crosvm).
#include <stdio.h>
#include "fixture.h"

int main(int argc, char **argv)
{
  static const uint8_t code[] = { 0x1f, 0x20, 0x03, 0xd5 };   // nop
  if (argc != 2 && argc != 3) { fprintf(stderr, "usage: fixture-kernel OUT [IDENTITY-OUT]\n"); return 2; }
  fx_image im = fx_kernel(FX_BASE, 0, code, sizeof code, 1);
  FILE *f = fopen(argv[1], "wb");
  if (!f || fwrite(im.data, 1, im.size, f) != im.size || fclose(f)) { fprintf(stderr, "%s: write failed\n", argv[1]); return 1; }
  free(im.data);
  if (argc == 3) {
    xl_adt_node *tree = fx_identity();
    const size_t size = xl_adt_flat_size(tree);
    uint8_t *flat = malloc(size);
    if (!flat) return 1;
    xl_adt_flatten(tree, flat);
    f = fopen(argv[2], "wb");
    if (!f || fwrite(flat, 1, size, f) != size || fclose(f)) { fprintf(stderr, "%s: write failed\n", argv[2]); return 1; }
    free(flat);
    xl_adt_free(tree);
  }
  return 0;
}
