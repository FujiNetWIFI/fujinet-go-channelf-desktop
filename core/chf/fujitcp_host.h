/* fujitcp_host.h -- what fujitcp_host.c adds to the staged fujitcp.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (matches the staged fujitcp.h it extends; copyright-holders Thomas Cherryhomes)
 */

#ifndef FUJITCP_HOST_H
#define FUJITCP_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wake a transaction blocked on the socket in another thread; it fails with
 * FB_ENOLINK and the socket is closed. Safe from any thread, and a no-op
 * when there is no socket. */
void fujitcp_abort(void);

/* The bytes on the socket, for the debugger's FujiNet trace: dir 1 a frame
 * the console sent (only what went out), dir 0 a frame read (a reply or a
 * push). Called on the thread running the transport: the cartridge's
 * worker, or the emulation thread in sync mode. fn NULL removes it; once
 * that returns, no call is in flight. */
typedef void (*fujitcp_trace_fn)(void *user, int dir, const uint8_t *buf, size_t len);
void fujitcp_set_trace(fujitcp_trace_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* FUJITCP_HOST_H */
