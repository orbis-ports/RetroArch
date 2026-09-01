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

/* ⚠ glsm_ctl, FOR A CORE THAT HAS NO glsm - AND WEAK, WHICH IS THE WHOLE TRICK.
 *
 * ps4/orbis_gl_forward.c declares glsm_ctl weak and null-checks it, which is correct C and is not
 * enough: an undefined weak symbol is exactly what create-fself refuses, so a GL core without glsm
 * linked cleanly and then got
 *
 *     Failed to build FSELF: missing library for symbol (glsm_ctl)
 *
 * Defining it WEAK here means a core that DOES build glsm still wins - its strong definition
 * overrides this one - while a core that does not gets a definition rather than a hole. Returning
 * 0 is the truthful answer to every GLSM_CTL_* query from a core with no glsm; orbis_gl_forward.c
 * then says so and points at orbis_gl_resolve_proc(). */
__attribute__((weak)) int glsm_ctl(int state, void *data);
__attribute__((weak)) int glsm_ctl(int state, void *data)
{ (void)state; (void)data; return 0; }

/* ⚠ AND THE FRONTEND'S LOGGER, BECAUSE dylib.o IS IN THE SHARED ARCHIVE AND NOW CALLS IT.
 *
 * ps4/build-cores.sh assembles liborbis-retro-common.a out of the FRONTEND's compiled
 * libretro-common objects, and libretro-common/dynamic/dylib.c is one of them. Its ORBIS arm - the
 * constructor table that stops a reloaded .prx running its initialisers twice - reports through
 * RARCH_LOG and ps4_log. Those live OUTSIDE libretro-common: RARCH_LOG in verbosity.c at the tree
 * root, ps4_log in the application framework. So the moment a core pulls dylib.o in, it needs two
 * symbols the archive cannot carry.
 *
 * Measured 2026-09-01, CI run 33489981683: melonDS DS references dylib_load for its libpcap probe,
 * so ld.lld extracted dylib.o and stopped with
 *
 *     ld.lld: error: undefined symbol: RARCH_LOG   >>> referenced by dylib.o:(dylib_load)
 *     ld.lld: error: undefined symbol: ps4_log     >>> referenced by dylib.o:(dylib_close)
 *
 * ⚠ AND IT PASSED LOCALLY, WHICH IS THE PART WORTH REMEMBERING. build-cores.sh caches
 * liborbis-retro-common.a and rebuilds it only when it is absent, so every local sweep since that
 * dylib.c change linked against an archive holding the PREVIOUS dylib.o. CI builds the archive from
 * scratch every run and saw the truth first.
 *
 * Pulling in the real ones is not an option: verbosity.o needs frontend_driver_attach_console and
 * would drag the frontend's driver into every core. These discard instead - a core has no netlog
 * socket and no verbosity state to log through, and the messages are about the FRONTEND loading
 * modules, which is not something a core does. */
void RARCH_LOG(const char *fmt, ...);
void ps4_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void ps4_log(const char *fmt, ...)   { (void)fmt; }
