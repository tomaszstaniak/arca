/* host shim: no task signals on the host; nothing is ever interrupted. */
#ifndef SHIM_PROTO_EXEC_H
#define SHIM_PROTO_EXEC_H
#define SIGBREAKF_CTRL_C (1UL << 12)
static unsigned long SetSignal(unsigned long n, unsigned long m) { (void)n; (void)m; return 0; }
#endif
