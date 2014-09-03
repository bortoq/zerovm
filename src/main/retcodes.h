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

#ifndef RETCODES_H_
#define RETCODES_H_

enum {
  /* internal zerovm return codes 1..64 */
  ZERR_OK = 0,

  /* 1..31 codes reserved for signals */
  ZERR_SIG_1 = 1, /* SIGHUP */
  ZERR_SIG_6 = 6, /* SIGABRT. usually triggered by assert */
  ZERR_SIG_8 = 8, /* SIGFPE */
  ZERR_SIG_11 = 11, /* SIGSEGV */
  ZERR_SIG_14 = 14, /* SIGALRM. session timeout */

  ZERR_IO = 32, /* setup i/o error */
  ZERR_MEM = 33, /* setup memory allocation error */
  ZERR_SYS = 34, /* setup system error */
  ZERR_NACL = 35, /* validation error, invalid syscall e.t.c. */
  ZERR_MFT_STX = 36, /* manifest syntax error. bad field */
  ZERR_MFT_DEF = 37, /* invalid definition in manifest. bad value */
  ZERR_SIG = 38, /* signal service error */
  ZERR_PLAT = 39, /* invalid platform */
  ZERR_CH_INV = 40, /* unsupported channel type */
  ZERR_CH_QUO = 41, /* channel cross the limits */
  ZERR_ESO = 64, /* esoteric error. very rare or should not happen */

  /* session related return codes 65..255 */
  SERR_IO = 65,
  SERR_ADDR = 66,
  SERR_SIG_8 = 67,
  SERR_SIG_11 = 68,
};

#endif /* RETCODES_H_ */
