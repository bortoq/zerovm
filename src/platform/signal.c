/*
 * Copyright (c) 2011 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/*
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <sys/mman.h>
#include "src/platform/signal.h"
#include "src/main/setup.h"
#include "src/main/config.h"
#include "src/main/tools.h"
#include "src/main/retcodes.h"

/*
 * This module is based on the Posix signal model. See:
 * http://www.opengroup.org/onlinepubs/009695399/functions/sigaction.html
 */

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*
 * TODO(noelallen) split these macros and conditional compiles
 * into architecture specific files. Bug #955
 */

/* Use 4K more than the minimum to allow breakpad to run. */
#define SIGNAL_STACK_SIZE (SIGSTKSZ + 4096)
#define STACK_GUARD_SIZE NACL_PAGESIZE

#define SIGNAL_COUNT ARRAY_SIZE(s_Signals)
static int s_Signals[] = {
  SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGBUS, SIGFPE,
  SIGSEGV, SIGSTKFLT, SIGABRT, SIGXFSZ, SIGXCPU,
  /* in the future SIGHUP will be used to restart session */
  SIGHUP,
  /*
   * for sake of determinism zvm makes all possible to prevent any
   * i/o errors
   */
  SIGPIPE,
  /* reserved for the future */
  SIGALRM,
  SIGTERM,
  /* reserved for the snapshot engine */
  SIGPWR
};

static struct sigaction s_OldActions[SIGNAL_COUNT];
static void *signal_stack;
static int busy = 1;

static void SignalStackRegister(void *stack)
{
  /*
   * If we set up signal handlers, we must ensure that any thread that
   * runs untrusted code has an alternate signal stack set up.  The
   * default for a new thread is to use the stack pointer from the
   * point at which the fault occurs, but it would not be safe to use
   * untrusted code's %esp/%rsp value.
   */
  stack_t st;
  st.ss_size = SIGNAL_STACK_SIZE;
  st.ss_sp = ((uint8_t *) stack) + STACK_GUARD_SIZE;
  st.ss_flags = 0;
  ZLOGFAIL(sigaltstack(&st, NULL) == -1,
      ZERR_SIG, "Failed to register signal stack");
}

static void SignalStackUnregister(void)
{
  /*
   * Unregister the signal stack in case a fault occurs between the
   * thread deallocating the signal stack and exiting.  Such a fault
   * could be unsafe if the address space were reallocated before the
   * fault, although that is unlikely.
   */
  stack_t st;
  st.ss_size = 0;
  st.ss_sp = NULL;
  st.ss_flags = SS_DISABLE;
  ZLOGFAIL(sigaltstack(&st, NULL) == -1,
      ZERR_SIG, "Failed to unregister signal stack");
}

/*
 * Allocates a stack suitable for passing to SignalStackRegister(),
 * for use as a stack for signal handlers. This can be called in any
 * thread. Stores the result in *result;
 */
static void SignalStackAllocate(void **result)
{
  /*
   * We use mmap() to allocate the signal stack for two reasons:
   *
   * 1) By page-aligning the memory allocation (which malloc() does
   * not do for small allocations), we avoid allocating any real
   * memory in the common case in which the signal handler is never
   * run.
   *
   * 2) We get to create a guard page, to guard against the unlikely
   * occurrence of the signal handler both overrunning and doing so in
   * an exploitable way.
   */
  uint8_t *stack = mmap(NULL, SIGNAL_STACK_SIZE + STACK_GUARD_SIZE,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ZLOGFAIL(stack == MAP_FAILED, ZERR_MEM, "failed to allocate signal stack");

  /* We assume that the stack grows downwards. */
  ZLOGFAIL(-1 == mprotect(stack, STACK_GUARD_SIZE, PROT_NONE),
      ZERR_MEM, "Failed to mprotect() the stack guard page");

  *result = stack;
}

static void SignalStackFree(void *stack)
{
  assert(stack != NULL);
  ZLOGFAIL(munmap(stack, SIGNAL_STACK_SIZE + STACK_GUARD_SIZE) == -1,
      ZERR_MEM, "Failed to munmap() signal stack");
}

static void FindAndRunHandler(int sig, siginfo_t *info, void *uc)
{
  int i;

  if(SignalHandlerFind(sig, uc) != NACL_SIGNAL_SEARCH) return;

  /* If we need to keep searching, try the old signal handler */
  for(i = 0; i < SIGNAL_COUNT; i++)
  {
    /* signal cannot be handled */
    if(s_Signals[i] != sig)
    {
      char msg[SIGNAL_STRLEN];
      g_snprintf(msg, SIGNAL_STRLEN, "Signal %d failed to be handled", sig);
      SessionDtor(EINTR, msg);
    }

    /* If this is a real sigaction pointer call the old handler */
    if(s_OldActions[i].sa_flags & SA_SIGINFO)
    {
      s_OldActions[i].sa_sigaction(sig, info, uc);
      break;
    }

    /* otherwise check if it is a real signal pointer and call the old signal */
    if((s_OldActions[i].sa_handler != SIG_DFL) && (s_OldActions[i].sa_handler != SIG_IGN))
    {
      s_OldActions[i].sa_handler(sig);
      break;
    }
  }
}

static void SignalCatch(int sig, siginfo_t *info, void *uc)
{
  busy = 1;
  FindAndRunHandler(sig, info, uc);
}

/* Assert that no signal handlers are registered */
static void SignalAssertNoHandlers()
{
  int i;

  for(i = 0; i < SIGNAL_COUNT; i++)
  {
    int signum = s_Signals[i];
    struct sigaction sa;

    ZLOGFAIL(-1 == sigaction(signum, NULL, &sa), ZERR_SIG, "sigaction() call failed");

    ZLOGFAIL((sa.sa_flags & SA_SIGINFO) != 0 ? sa.sa_sigaction != NULL
        : (sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN), ZERR_SIG,
        "A signal handler is registered for signal %d. Did Breakpad register this?", signum);
  }
}

void SignalHandlerInitPlatform()
{
  struct sigaction sa;
  int i;

  /* fail if another handler(s) already registered */
  SignalAssertNoHandlers();

  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = SignalCatch;
  sa.sa_flags = SA_ONSTACK | SA_SIGINFO;

  /* Mask all exceptions we catch to prevent re-entry */
  for(i = 0; i < SIGNAL_COUNT; i++)
    sigaddset(&sa.sa_mask, s_Signals[i]);

  /* Install all handlers */
  for(i = 0; i < SIGNAL_COUNT; i++)
    ZLOGFAIL(0 != sigaction(s_Signals[i], &sa, &s_OldActions[i]),
        ZERR_SIG, "Failed to install handler for %d", s_Signals[i]);

  /* allocate and register signal stack */
  SignalStackAllocate(&signal_stack);
  SignalStackRegister(signal_stack);
  busy = 0;
}

void SignalHandlerFiniPlatform()
{
  int i;

  /* Remove all handlers */
  for(i = 0; i < SIGNAL_COUNT; i++)
    sigaction(s_Signals[i], &s_OldActions[i], NULL);

  /* release signal stack if not busy */
  if(!busy)
  {
    SignalStackUnregister();
    SignalStackFree(signal_stack);
  }
}
