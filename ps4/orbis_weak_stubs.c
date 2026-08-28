/* RetroArch - A frontend for libretro.
 *
 * Definitions for symbols a module may reference weakly and never get.
 *
 * ⚠ WHY THIS IS NEEDED AT ALL. ld.lld is happy to leave an UNDEFINED WEAK symbol undefined - that
 * is what weak means, and at run time it would read as NULL. create-fself is not: it maps every
 * remaining undefined symbol to a library NID, finds no library that exports this one, and
 * refuses the whole module with
 *
 *     Failed to build FSELF: missing library for symbol (ZSTD_trace_decompress_end)
 *
 * ⚠ AND IT REFUSES WITH EXIT STATUS 0, so a build script that trusts the exit code sees success
 * and no .prx. The harness checks for the file instead.
 *
 * The alternative - `--defsym=NAME=0` - would satisfy create-fself and leave a call to address
 * zero for anything that does not null-check first. These are zstd's tracing hooks and a no-op is
 * their documented off state, so a real definition is both safer and more honest.
 *
 * This file is compiled into the core-build fallback archive, so a module picks it up only if it
 * has one of these undefined and nothing else provides it.
 */

/* zstd's trace context is an unsigned long long; the trace struct is opaque here because nothing
 * in this file looks inside it. */
unsigned long long ZSTD_trace_compress_begin(const void *cctx);
void               ZSTD_trace_compress_end(unsigned long long ctx, const void *trace);
unsigned long long ZSTD_trace_decompress_begin(const void *dctx);
void               ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace);

unsigned long long ZSTD_trace_compress_begin(const void *cctx) { (void)cctx; return 0; }
void ZSTD_trace_compress_end(unsigned long long ctx, const void *trace)
{ (void)ctx; (void)trace; }
unsigned long long ZSTD_trace_decompress_begin(const void *dctx) { (void)dctx; return 0; }
void ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace)
{ (void)ctx; (void)trace; }
