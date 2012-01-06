/*
 * Copyright (c) 2011 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "src/platform/nacl_sync.h"

int NaClMutexCtor(struct NaClMutex *mp) {
  if (0 != pthread_mutex_init(&mp->mu, (pthread_mutexattr_t *) 0)) {
    return 0;
  }
  return 1;
}

void NaClMutexDtor(struct NaClMutex *mp) {
  pthread_mutex_destroy(&mp->mu);
}

NaClSyncStatus NaClMutexLock(struct NaClMutex *mp) {
  pthread_mutex_lock(&mp->mu);
  return NACL_SYNC_OK;
}

NaClSyncStatus NaClMutexUnlock(struct NaClMutex *mp) {
  pthread_mutex_unlock(&mp->mu);
  return NACL_SYNC_OK;
}
