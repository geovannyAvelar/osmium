/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gdal_setup.h"

#include <cpl_conv.h>
#include <gdal.h>
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init(void) {
  GDALAllRegister();
  CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
}

void osmium_gdal_init(void) {
  pthread_once(&once, init);
}
