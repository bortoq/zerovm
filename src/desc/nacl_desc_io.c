/*
 * Copyright (c) 2011 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * NaCl Service Runtime.  I/O Descriptor / Handle abstraction.  Memory
 * mapping using descriptors.
 */

#include "src/desc/nacl_desc_effector.h"
#include "src/desc/nacl_desc_io.h"
#include "src/platform/nacl_find_addrsp.h"
#include "src/platform/nacl_log.h"
#include "src/service_runtime/include/sys/errno.h"
#include "src/service_runtime/include/sys/fcntl.h"
#include "src/service_runtime/include/sys/mman.h"
#include "src/service_runtime/internal_errno.h"
#include "src/service_runtime/nacl_config.h"

/*
 * This file contains the implementation for the NaClIoDesc subclass
 * of NaClDesc.
 *
 * NaClDescIoDesc is the subclass that wraps host-OS descriptors
 * provided by NaClHostDesc (which gives an OS-independent abstraction
 * for host-OS descriptors).
 */

static struct NaClDescVtbl const kNaClDescIoDescVtbl;  /* fwd */
/*
 * Takes ownership of hd, will close in Dtor.
 */
int NaClDescIoDescCtor(struct NaClDescIoDesc  *self,
                       struct NaClHostDesc    *hd) {
  struct NaClDesc *basep = (struct NaClDesc *) self;

  basep->base.vtbl = (struct NaClRefCountVtbl const *) NULL;
  if (!NaClDescCtor(basep)) {
    return 0;
  }
  self->hd = hd;
  basep->base.vtbl = (struct NaClRefCountVtbl const *) &kNaClDescIoDescVtbl;
  return 1;
}

static void NaClDescIoDescDtor(struct NaClRefCount *vself) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  NaClLog(4, "NaClDescIoDescDtor(0x%08"NACL_PRIxPTR").\n",
          (uintptr_t) vself);
  NaClHostDescClose(self->hd);
  free(self->hd);
  self->hd = NULL;
  vself->vtbl = (struct NaClRefCountVtbl const *) &kNaClDescVtbl;
  (*vself->vtbl->Dtor)(vself);
}

struct NaClDescIoDesc *NaClDescIoDescMake(struct NaClHostDesc *nhdp) {
  struct NaClDescIoDesc *ndp;

  ndp = malloc(sizeof *ndp);
  if (NULL == ndp) {
    NaClLog(LOG_FATAL,
            "NaClDescIoDescMake: no memory for 0x%08"NACL_PRIxPTR"\n",
            (uintptr_t) nhdp);
  }
  if (!NaClDescIoDescCtor(ndp, nhdp)) {
    NaClLog(LOG_FATAL,
            ("NaClDescIoDescMake:"
             " NaClDescIoDescCtor(0x%08"NACL_PRIxPTR",0x%08"NACL_PRIxPTR
             ") failed\n"),
            (uintptr_t) ndp,
            (uintptr_t) nhdp);
  }
  return ndp;
}

static uintptr_t NaClDescIoDescMap(struct NaClDesc         *vself,
                                   struct NaClDescEffector *effp,
                                   void                    *start_addr,
                                   size_t                  len,
                                   int                     prot,
                                   int                     flags,
                                   nacl_off64_t            offset) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;
  int                   rv;
  uintptr_t             status;
  uintptr_t             addr;
  uintptr_t             end_addr;
  nacl_off64_t          tmp_off;

  /*
   * prot must be PROT_NONE or a combination of PROT_{READ|WRITE}
   */
  if (0 != (~(NACL_ABI_PROT_READ | NACL_ABI_PROT_WRITE) & prot)) {
    NaClLog(LOG_INFO,
            ("NaClDescIoDescMap: prot has other bits"
             " than PROT_{READ|WRITE}\n"));
    return -NACL_ABI_EINVAL;
  }

  if (0 == (NACL_ABI_MAP_FIXED & flags) && NULL == start_addr) {
    NaClLog(LOG_INFO,
            ("NaClDescIoDescMap: Mapping not NACL_ABI_MAP_FIXED"
             " but start_addr is NULL\n"));
  }

  if (0 == (NACL_ABI_MAP_FIXED & flags)) {
    if (!NaClFindAddressSpace(&addr, len)) {
      NaClLog(1, "NaClDescIoDescMap: no address space?\n");
      return -NACL_ABI_ENOMEM;
    }
    start_addr = (void *) addr;
  }
  flags |= NACL_ABI_MAP_FIXED;

  for (addr = (uintptr_t) start_addr,
           end_addr = addr + len,
           tmp_off = offset;
       addr < end_addr;
       addr += NACL_MAP_PAGESIZE,
           tmp_off += NACL_MAP_PAGESIZE) {
    size_t map_size;

    if (0 != (rv = (*effp->vtbl->UnmapMemory)(effp,
                                              addr,
                                              NACL_MAP_PAGESIZE))) {
      NaClLog(LOG_FATAL,
              ("NaClDescIoDescMap: error %d --"
               " could not unmap 0x%08"NACL_PRIxPTR
               ", length 0x%"NACL_PRIxS"\n"),
              rv,
              addr,
              (size_t) NACL_MAP_PAGESIZE);
    }

    map_size = end_addr - addr;
    if (map_size > NACL_MAP_PAGESIZE) {
      map_size = NACL_MAP_PAGESIZE;
    }
    status = NaClHostDescMap((NULL == self) ? NULL : self->hd,
                             (void *) addr,
                             map_size,
                             prot,
                             flags,
                             tmp_off);
    if (NACL_MAP_FAILED == (void *) status) {
      return -NACL_ABI_E_MOVE_ADDRESS_SPACE;
    }
  }
  return (uintptr_t) start_addr;
}

uintptr_t NaClDescIoDescMapAnon(struct NaClDescEffector *effp,
                                void                    *start_addr,
                                size_t                  len,
                                int                     prot,
                                int                     flags,
                                nacl_off64_t            offset) {
  return NaClDescIoDescMap((struct NaClDesc *) NULL, effp, start_addr, len,
                           prot, flags, offset);
}

static int NaClDescIoDescUnmapCommon(struct NaClDesc         *vself,
                                     struct NaClDescEffector *effp,
                                     void                    *start_addr,
                                     size_t                  len,
                                     int                     safe_mode) {
  int status;

  UNREFERENCED_PARAMETER(vself);
  UNREFERENCED_PARAMETER(effp);

  if (safe_mode) {
    status = NaClHostDescUnmap(start_addr, len);
  } else {
    status = NaClHostDescUnmapUnsafe(start_addr, len);
  }

  return status;
}

/*
 * NB: User code should never be able to invoke the Unsafe method.
 */
static int NaClDescIoDescUnmapUnsafe(struct NaClDesc         *vself,
                                     struct NaClDescEffector *effp,
                                     void                    *start_addr,
                                     size_t                  len) {
  return NaClDescIoDescUnmapCommon(vself, effp, start_addr, len, 0);
}

static int NaClDescIoDescUnmap(struct NaClDesc         *vself,
                               struct NaClDescEffector *effp,
                               void                    *start_addr,
                               size_t                  len) {
  return NaClDescIoDescUnmapCommon(vself, effp, start_addr, len, 1);
}

static ssize_t NaClDescIoDescRead(struct NaClDesc          *vself,
                                  void                     *buf,
                                  size_t                   len) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  return NaClHostDescRead(self->hd, buf, len);
}

static ssize_t NaClDescIoDescWrite(struct NaClDesc         *vself,
                                   void const              *buf,
                                   size_t                  len) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  return NaClHostDescWrite(self->hd, buf, len);
}

static nacl_off64_t NaClDescIoDescSeek(struct NaClDesc          *vself,
                                       nacl_off64_t             offset,
                                       int                      whence) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  return NaClHostDescSeek(self->hd, offset, whence);
}

static int NaClDescIoDescIoctl(struct NaClDesc         *vself,
                               int                     request,
                               void                    *arg) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  return NaClHostDescIoctl(self->hd, request, arg);
}

static int NaClDescIoDescFstat(struct NaClDesc         *vself,
                               struct nacl_abi_stat    *statbuf) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;
  int                   rv;
  nacl_host_stat_t      hstatbuf;

  rv = NaClHostDescFstat(self->hd, &hstatbuf);
  if (0 != rv) {
    return rv;
  }
  return NaClAbiStatHostDescStatXlateCtor(statbuf, &hstatbuf);
}

static int NaClDescIoDescExternalizeSize(struct NaClDesc *vself,
                                         size_t          *nbytes,
                                         size_t          *nhandles) {
  UNREFERENCED_PARAMETER(vself);

  *nbytes = 0;
  *nhandles = 1;
  return 0;
}

static int NaClDescIoDescExternalize(struct NaClDesc           *vself,
                                     struct NaClDescXferState  *xfer) {
  struct NaClDescIoDesc *self = (struct NaClDescIoDesc *) vself;

  *xfer->next_handle++ = self->hd->d;
  return 0;
}

static struct NaClDescVtbl const kNaClDescIoDescVtbl = {
  {
    NaClDescIoDescDtor,
  },
  NaClDescIoDescMap,
  NaClDescIoDescUnmapUnsafe,
  NaClDescIoDescUnmap,
  NaClDescIoDescRead,
  NaClDescIoDescWrite,
  NaClDescIoDescSeek,
  NaClDescIoDescIoctl,
  NaClDescIoDescFstat,
  NaClDescGetdentsNotImplemented,
  NACL_DESC_HOST_IO,
  NaClDescIoDescExternalizeSize,
  NaClDescIoDescExternalize,
  NaClDescLockNotImplemented,
  NaClDescTryLockNotImplemented,
  NaClDescUnlockNotImplemented,
  NaClDescWaitNotImplemented,
  NaClDescTimedWaitAbsNotImplemented,
  NaClDescSignalNotImplemented,
  NaClDescBroadcastNotImplemented,
  NaClDescSendMsgNotImplemented,
  NaClDescRecvMsgNotImplemented,
  NaClDescConnectAddrNotImplemented,
  NaClDescAcceptConnNotImplemented,
  NaClDescPostNotImplemented,
  NaClDescSemWaitNotImplemented,
  NaClDescGetValueNotImplemented,
};

/* set *out_desc to struct NaClDescIo * output */
int NaClDescIoInternalize(struct NaClDesc           **out_desc,
                          struct NaClDescXferState  *xfer) {
  int                   rv;
  NaClHandle            h;
  int                   d;
  struct NaClHostDesc   *nhdp;
  struct NaClDescIoDesc *ndidp;

  rv = -NACL_ABI_EIO;  /* catch-all */
  h = NACL_INVALID_HANDLE;
  nhdp = NULL;
  ndidp = NULL;

  if (xfer->next_handle == xfer->handle_buffer_end) {
    rv = -NACL_ABI_EIO;
    goto cleanup;
  }
  nhdp = malloc(sizeof *nhdp);
  if (NULL == nhdp) {
    rv = -NACL_ABI_ENOMEM;
    goto cleanup;
  }
  ndidp = malloc(sizeof *ndidp);
  if (!ndidp) {
    rv = -NACL_ABI_ENOMEM;
    goto cleanup;
  }
  h = *xfer->next_handle;
  *xfer->next_handle++ = NACL_INVALID_HANDLE;
  d = h;
  /*
   * We mark it as read/write, but don't really know for sure until we
   * try to make those syscalls (in which case we'd get EBADF).
   */
  if ((rv = NaClHostDescPosixTake(nhdp, d, NACL_ABI_O_RDWR)) < 0) {
    goto cleanup;
  }
  h = NACL_INVALID_HANDLE;  /* nhdp took ownership of h */

  if (!NaClDescIoDescCtor(ndidp, nhdp)) {
    rv = -NACL_ABI_ENOMEM;
    goto cleanup_hd_dtor;
  }
  /*
   * ndidp took ownership of nhdp, now give ownership of ndidp to caller.
   */
  *out_desc = (struct NaClDesc *) ndidp;
  rv = 0;
cleanup_hd_dtor:
  if (rv < 0) {
    (void) NaClHostDescClose(nhdp);
  }
cleanup:
  if (rv < 0) {
    free(nhdp);
    free(ndidp);
    if (NACL_INVALID_HANDLE != h) {
      (void) NaClClose(h);
    }
  }
  return rv;
}
