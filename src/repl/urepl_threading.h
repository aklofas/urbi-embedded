/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_threading.h — REPL threading abstraction (v0.9.4-followup)
 *
 * One header, two type expansions:
 *   - POSIX builds (default):       urbi_mutex_t = pthread_mutex_t, etc.
 *   - URBI_REPL_COOPERATIVE_ONLY=1: urbi_mutex_t = empty stub, macros no-op
 *
 * Embedders MUST link with the same URBI_REPL_COOPERATIVE_ONLY setting the
 * library was built with — struct layouts (UReplServer, UReplReader,
 * UReplQueue, UReplRingbuf) differ between modes. Same trap class as
 * URBI_FLOAT_TYPE. */
#ifndef UREPL_THREADING_H
#define UREPL_THREADING_H

#ifdef URBI_REPL_COOPERATIVE_ONLY
  /* Single-thread, embedder-driven serve_step. No pthread, no eventfd,
   * no sockets — urepl_listener.c and TCP/Unix/PTY transports are
   * filtered out of REPL_SRCS in Makefile.                              */

  typedef struct { char _unused; } urbi_mutex_t;
  typedef struct { char _unused; } urbi_cond_t;
  typedef struct { char _unused; } urbi_thread_t;
  typedef void *(*urbi_thread_fn_t)(void *);

  #define UREPL_MUTEX_INIT(m)     0
  #define UREPL_MUTEX_DESTROY(m)  ((void)0)
  #define UREPL_MUTEX_LOCK(m)     ((void)0)
  #define UREPL_MUTEX_UNLOCK(m)   ((void)0)

  #define UREPL_COND_INIT(c)      0
  #define UREPL_COND_DESTROY(c)   ((void)0)
  #define UREPL_COND_SIGNAL(c)      ((void)0)
  #define UREPL_COND_BROADCAST(c)   ((void)0)
  #define UREPL_COND_WAIT(c, m)     ((void)0)  /* never blocks; caller polls */

  #define UREPL_THREAD_CREATE(t, fn, ud)  (-1) /* always fails — caller MUST gate */
  #define UREPL_THREAD_JOIN(t)            ((void)0)

#else  /* POSIX threads — the default for Linux/macOS/FreeBSD hosts */
  #include <pthread.h>

  typedef pthread_mutex_t urbi_mutex_t;
  typedef pthread_cond_t  urbi_cond_t;
  typedef pthread_t       urbi_thread_t;
  typedef void *(*urbi_thread_fn_t)(void *);

  #define UREPL_MUTEX_INIT(m)     pthread_mutex_init((m), NULL)
  #define UREPL_MUTEX_DESTROY(m)  pthread_mutex_destroy(m)
  #define UREPL_MUTEX_LOCK(m)     pthread_mutex_lock(m)
  #define UREPL_MUTEX_UNLOCK(m)   pthread_mutex_unlock(m)

  #define UREPL_COND_INIT(c)      pthread_cond_init((c), NULL)
  #define UREPL_COND_DESTROY(c)   pthread_cond_destroy(c)
  #define UREPL_COND_SIGNAL(c)      pthread_cond_signal(c)
  #define UREPL_COND_BROADCAST(c)   pthread_cond_broadcast(c)
  #define UREPL_COND_WAIT(c, m)   pthread_cond_wait((c), (m))

  #define UREPL_THREAD_CREATE(t, fn, ud)  pthread_create((t), NULL, (fn), (ud))
  #define UREPL_THREAD_JOIN(t)            pthread_join((t), NULL)
#endif

#endif /* UREPL_THREADING_H */
