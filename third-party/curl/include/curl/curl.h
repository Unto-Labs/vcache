/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
// The part of libcurl's <curl/curl.h> that src/storage/curl_api.h needs, for
// building where curl's headers are not installed. The Makefile puts this
// directory on the include path only then; see third-party/curl/PROVENANCE.md.
//
// No functions are declared: vcache resolves every entry point with dlsym, so
// it needs only the types and constants. The values are libcurl's ABI, fixed
// for the life of soname 4, and are written the way curl.h spells them so the
// two can be compared line by line.
//
// C++ only. The enums take a fixed underlying type because CURLcode lists one
// enumerator here: without it, every error code libcurl returns would fall
// outside the enum's range, which C++ leaves undefined.
#pragma once

#include <cstdint>

typedef void CURL;

struct curl_slist {
  char* data;
  struct curl_slist* next;
};

// 64 bits on every platform libcurl supports.
typedef int64_t curl_off_t;

enum CURLcode : int {
  CURLE_OK = 0,
};

#define CURLOPTTYPE_LONG          0
#define CURLOPTTYPE_OBJECTPOINT   10000
#define CURLOPTTYPE_FUNCTIONPOINT 20000
#define CURLOPTTYPE_OFF_T         30000

enum CURLoption : int {
  CURLOPT_WRITEDATA        = CURLOPTTYPE_OBJECTPOINT + 1,
  CURLOPT_URL              = CURLOPTTYPE_OBJECTPOINT + 2,
  CURLOPT_READDATA         = CURLOPTTYPE_OBJECTPOINT + 9,
  CURLOPT_WRITEFUNCTION    = CURLOPTTYPE_FUNCTIONPOINT + 11,
  CURLOPT_READFUNCTION     = CURLOPTTYPE_FUNCTIONPOINT + 12,
  CURLOPT_TIMEOUT          = CURLOPTTYPE_LONG + 13,
  CURLOPT_HTTPHEADER       = CURLOPTTYPE_OBJECTPOINT + 23,
  CURLOPT_HEADERDATA       = CURLOPTTYPE_OBJECTPOINT + 29,
  CURLOPT_CUSTOMREQUEST    = CURLOPTTYPE_OBJECTPOINT + 36,
  CURLOPT_UPLOAD           = CURLOPTTYPE_LONG + 46,
  CURLOPT_FOLLOWLOCATION   = CURLOPTTYPE_LONG + 52,
  CURLOPT_CONNECTTIMEOUT   = CURLOPTTYPE_LONG + 78,
  CURLOPT_HEADERFUNCTION   = CURLOPTTYPE_FUNCTIONPOINT + 79,
  CURLOPT_NOSIGNAL         = CURLOPTTYPE_LONG + 99,
  CURLOPT_INFILESIZE_LARGE = CURLOPTTYPE_OFF_T + 115,
};

#define CURLINFO_LONG 0x200000

enum CURLINFO : int {
  CURLINFO_RESPONSE_CODE = CURLINFO_LONG + 2,
};

#define CURL_GLOBAL_SSL     (1 << 0)
#define CURL_GLOBAL_WIN32   (1 << 1)
#define CURL_GLOBAL_ALL     (CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32)
#define CURL_GLOBAL_DEFAULT CURL_GLOBAL_ALL
