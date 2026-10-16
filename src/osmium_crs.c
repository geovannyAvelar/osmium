/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_crs.h"

#include "proj/projection.h"

gboolean osmium_crs_describe(const char *definition, char **name, char **code, gboolean *geographic) {
  OsmiumCrsInfo info;

  g_return_val_if_fail(definition != NULL, FALSE);

  if (!osmium_crs_describe_internal(definition, &info)) {
    return FALSE;
  }

  if (name) {
    *name = g_strdup(info.name);
  }
  if (code) {
    *code = g_strdup(info.code);
  }
  if (geographic) {
    *geographic = info.geographic;
  }
  osmium_crs_info_clear(&info);
  return TRUE;
}

void osmium_set_proj_network_enabled(gboolean enabled) {
  osmium_proj_set_network_enabled(enabled);
}

gboolean osmium_get_proj_network_enabled(void) {
  return osmium_proj_get_network_enabled();
}
