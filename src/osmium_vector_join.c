/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_vector_layer.h"
#include "gdal_setup.h"

#include "osmium_vector_layer_private.h"
#include "vector/vector_data.h"

#include <gdal.h>
#include <ogr_api.h>
#include <math.h>
#include <string.h>

typedef struct {
  OsmiumJoin *config;
  OsmiumAttributeTable *table;
  GHashTable *index;
  guint *columns;
  guint n_columns;
  char **names;
  char *error;
} JoinState;

OsmiumJoin *osmium_join_copy(const OsmiumJoin *join) {
  OsmiumJoin *copy = g_new0(OsmiumJoin, 1);

  copy->path = g_strdup(join->path);
  copy->target_field = g_strdup(join->target_field);
  copy->source_field = g_strdup(join->source_field);
  copy->prefix = g_strdup(join->prefix);
  copy->fields = g_strdupv(join->fields);
  return copy;
}

void osmium_join_free(OsmiumJoin *join) {
  if (join == NULL) {
    return;
  }
  g_free(join->path);
  g_free(join->target_field);
  g_free(join->source_field);
  g_free(join->prefix);
  g_strfreev(join->fields);
  g_free(join);
}

static OsmiumFeatureFieldType field_type_from_ogr(OGRFieldDefnH defn) {
  switch (OGR_Fld_GetType(defn)) {
  case OFTInteger:
  case OFTInteger64:
    return OSMIUM_FEATURE_FIELD_INTEGER;
  case OFTReal:
    return OSMIUM_FEATURE_FIELD_REAL;
  case OFTString:
    return OSMIUM_FEATURE_FIELD_STRING;
  case OFTDate:
  case OFTDateTime:
    return OSMIUM_FEATURE_FIELD_DATE;
  default:
    return OSMIUM_FEATURE_FIELD_OTHER;
  }
}

OsmiumAttributeTable *osmium_table_open(const char *path, GError **error) {
  osmium_gdal_init();

  GDALDatasetH dataset = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL);
  OGRLayerH layer = dataset != NULL ? GDALDatasetGetLayer(dataset, 0) : NULL;

  if (layer == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the table %s", path);
    if (dataset != NULL) {
      GDALClose(dataset);
    }
    return NULL;
  }

  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
  const int n_fields = OGR_FD_GetFieldCount(defn);
  OsmiumAttributeTable *table = g_new0(OsmiumAttributeTable, 1);
  GArray *fids = g_array_new(FALSE, FALSE, sizeof(gint64));
  GPtrArray *cells = g_ptr_array_new();
  OGRFeatureH feature;

  table->n_fields = (guint)n_fields;
  table->names = g_new0(char *, n_fields + 1);
  table->types = g_new0(OsmiumFeatureFieldType, MAX(n_fields, 1));
  for (int i = 0; i < n_fields; i++) {
    OGRFieldDefnH field = OGR_FD_GetFieldDefn(defn, i);

    table->names[i] = g_strdup(OGR_Fld_GetNameRef(field));
    table->types[i] = field_type_from_ogr(field);
  }

  OGR_L_ResetReading(layer);
  while ((feature = OGR_L_GetNextFeature(layer)) != NULL) {
    const gint64 fid = OGR_F_GetFID(feature);

    g_array_append_val(fids, fid);
    for (int i = 0; i < n_fields; i++) {
      const char *value = OGR_F_IsFieldSetAndNotNull(feature, i) ? OGR_F_GetFieldAsString(feature, i) : "";

      if (OGR_F_IsFieldSetAndNotNull(feature, i) && OGR_Fld_GetType(OGR_FD_GetFieldDefn(defn, i)) == OFTReal) {
        char text[G_ASCII_DTOSTR_BUF_SIZE];

        g_ascii_formatd(text, sizeof(text), "%.15g", OGR_F_GetFieldAsDouble(feature, i));
        g_ptr_array_add(cells, g_strdup(text));
      } else {
        g_ptr_array_add(cells, g_strdup(value != NULL ? value : ""));
      }
    }
    OGR_F_Destroy(feature);
  }
  GDALClose(dataset);

  table->n_rows = fids->len;
  table->fids = g_memdup2(fids->data, MAX(fids->len, 1) * sizeof(gint64));
  table->cells = (char **)g_ptr_array_free(cells, FALSE);
  g_array_unref(fids);
  return table;
}

static char *normalize_key(const char *text) {
  g_autofree char *trimmed = g_strstrip(g_strdup(text != NULL ? text : ""));
  char *end = NULL;
  double value;

  if (trimmed[0] == '\0') {
    return NULL;
  }
  value = g_ascii_strtod(trimmed, &end);
  if (end != trimmed && *end == '\0' && isfinite(value) && value == floor(value) && fabs(value) < 1e15) {
    return g_strdup_printf("%.0f", value);
  }
  return g_strdup(trimmed);
}

static guint column_index(const OsmiumAttributeTable *table, const char *name) {
  for (guint i = 0; name != NULL && i < table->n_fields; i++) {
    if (g_strcmp0(table->names[i], name) == 0) {
      return i;
    }
  }
  return G_MAXUINT;
}

static void join_state_free(gpointer data) {
  JoinState *state = data;

  osmium_join_free(state->config);
  osmium_attribute_table_free(state->table);
  g_clear_pointer(&state->index, g_hash_table_unref);
  g_free(state->columns);
  g_strfreev(state->names);
  g_free(state->error);
  g_free(state);
}

static JoinState *join_state_new(const OsmiumJoin *config, GPtrArray *taken_names) {
  JoinState *state = g_new0(JoinState, 1);
  g_autoptr(GError) error = NULL;
  guint key;
  g_autofree char *base = g_path_get_basename(config->path != NULL ? config->path : "join");
  g_autofree char *stem = g_strdup(base);
  char *dot = strrchr(stem, '.');
  g_autofree char *prefix = NULL;
  GPtrArray *columns = g_ptr_array_new();
  GPtrArray *names = g_ptr_array_new();

  state->config = osmium_join_copy(config);
  state->table = osmium_table_open(config->path, &error);
  if (dot != NULL) {
    *dot = '\0';
  }
  prefix = g_strdup(config->prefix != NULL ? config->prefix : "");
  if (config->prefix == NULL) {
    g_free(prefix);
    prefix = g_strdup_printf("%s_", stem);
  }

  if (state->table == NULL) {
    state->error = g_strdup(error != NULL ? error->message : "Could not read the table");
    state->columns = NULL;
    state->names = g_new0(char *, 1);
    g_ptr_array_free(columns, TRUE);
    g_ptr_array_free(names, TRUE);
    return state;
  }

  key = column_index(state->table, config->source_field);
  if (key == G_MAXUINT) {
    state->error = g_strdup_printf("The join table has no field “%s”", config->source_field != NULL ? config->source_field : "");
  } else {
    state->index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint r = 0; r < state->table->n_rows; r++) {
      char *normalized = normalize_key(state->table->cells[(gsize)r * state->table->n_fields + key]);

      if (normalized != NULL && !g_hash_table_contains(state->index, normalized)) {
        g_hash_table_insert(state->index, normalized, GUINT_TO_POINTER(r + 1));
      } else {
        g_free(normalized);
      }
    }
  }

  for (guint c = 0; c < state->table->n_fields; c++) {
    const char *name = state->table->names[c];
    g_autofree char *virtual_name = NULL;
    guint suffix = 2;

    if (c == key) {
      continue;
    }
    if (config->fields != NULL && !g_strv_contains((const char *const *)config->fields, name)) {
      continue;
    }

    virtual_name = g_strconcat(prefix, name, NULL);
    while (g_ptr_array_find_with_equal_func(taken_names, virtual_name, g_str_equal, NULL)) {
      g_free(virtual_name);
      virtual_name = g_strdup_printf("%s%s_%u", prefix, name, suffix++);
    }
    g_ptr_array_add(taken_names, g_strdup(virtual_name));
    g_ptr_array_add(columns, GUINT_TO_POINTER(c + 1));
    g_ptr_array_add(names, g_steal_pointer(&virtual_name));
  }

  state->n_columns = columns->len;
  state->columns = g_new0(guint, MAX(columns->len, 1));
  state->names = g_new0(char *, names->len + 1);
  for (guint i = 0; i < columns->len; i++) {
    state->columns[i] = GPOINTER_TO_UINT(g_ptr_array_index(columns, i)) - 1;
    state->names[i] = g_ptr_array_index(names, i);
  }
  g_ptr_array_free(columns, TRUE);
  g_ptr_array_free(names, TRUE);
  return state;
}

static GPtrArray *real_field_names(OsmiumVectorLayer *self) {
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) fields = osmium_vector_layer_get_fields(self);

  for (guint i = 0; fields != NULL && i < fields->len; i++) {
    g_ptr_array_add(names, g_strdup(((OsmiumFeatureField *)g_ptr_array_index(fields, i))->name));
  }
  return names;
}

void osmium_join_rebuild(OsmiumVectorLayer *self) {
  g_autoptr(GPtrArray) taken = real_field_names(self);
  GPtrArray *states = g_ptr_array_new_with_free_func(join_state_free);

  for (guint i = 0; self->join_configs != NULL && i < self->join_configs->len; i++) {
    g_ptr_array_add(states, join_state_new(g_ptr_array_index(self->join_configs, i), taken));
  }
  if (self->join_states != NULL) {
    g_ptr_array_unref(self->join_states);
  }
  self->join_states = states;
}

gboolean osmium_vector_layer_set_joins(OsmiumVectorLayer *self, GPtrArray *joins, GError **error) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), FALSE);

  gboolean ok = TRUE;

  if (self->join_configs != NULL) {
    g_ptr_array_unref(self->join_configs);
  }
  self->join_configs = g_ptr_array_new_with_free_func((GDestroyNotify)osmium_join_free);
  for (guint i = 0; joins != NULL && i < joins->len; i++) {
    g_ptr_array_add(self->join_configs, osmium_join_copy(g_ptr_array_index(joins, i)));
  }

  osmium_join_rebuild(self);
  osmium_vector_layer_invalidate_columns(self);

  for (guint i = 0; i < self->join_states->len; i++) {
    const JoinState *state = g_ptr_array_index(self->join_states, i);

    if (state->error != NULL && ok) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, state->error);
      ok = FALSE;
    }
  }
  osmium_vector_layer_refresh(self);
  return ok;
}

GPtrArray *osmium_vector_layer_dup_joins(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify)osmium_join_free);

  for (guint i = 0; self->join_configs != NULL && i < self->join_configs->len; i++) {
    g_ptr_array_add(copy, osmium_join_copy(g_ptr_array_index(self->join_configs, i)));
  }
  return copy;
}

char *osmium_vector_layer_dup_join_error(OsmiumVectorLayer *self) {
  for (guint i = 0; self->join_states != NULL && i < self->join_states->len; i++) {
    const JoinState *state = g_ptr_array_index(self->join_states, i);

    if (state->error != NULL) {
      return g_strdup(state->error);
    }
  }
  return NULL;
}

guint osmium_join_count_fields(OsmiumVectorLayer *self) {
  guint count = 0;

  for (guint i = 0; self->join_states != NULL && i < self->join_states->len; i++) {
    count += ((JoinState *)g_ptr_array_index(self->join_states, i))->n_columns;
  }
  return count;
}

static JoinState *find_state(OsmiumVectorLayer *self, const char *name, guint *which) {
  for (guint i = 0; self->join_states != NULL && i < self->join_states->len; i++) {
    JoinState *state = g_ptr_array_index(self->join_states, i);

    for (guint f = 0; f < state->n_columns; f++) {
      if (g_strcmp0(state->names[f], name) == 0) {
        *which = f;
        return state;
      }
    }
  }
  return NULL;
}

const char *osmium_join_field_name(OsmiumVectorLayer *self, guint index, OsmiumFeatureFieldType *type) {
  for (guint i = 0; self->join_states != NULL && i < self->join_states->len; i++) {
    const JoinState *state = g_ptr_array_index(self->join_states, i);

    if (index < state->n_columns) {
      if (type != NULL) {
        *type = state->table->types[state->columns[index]];
      }
      return state->names[index];
    }
    index -= state->n_columns;
  }
  return NULL;
}

gboolean osmium_vector_layer_is_joined_field(OsmiumVectorLayer *self, const char *name) {
  guint which;

  return find_state(self, name, &which) != NULL;
}

GPtrArray *osmium_join_column(OsmiumVectorLayer *self, const char *name) {
  guint which;
  JoinState *state = find_state(self, name, &which);

  if (state == NULL || self->data == NULL) {
    return NULL;
  }

  GPtrArray *keys = state->index != NULL ? osmium_vector_layer_column(self, state->config->target_field) : NULL;
  GPtrArray *column = g_ptr_array_new_with_free_func(g_free);

  for (size_t i = 0; i < self->data->n_features; i++) {
    const char *value = "";

    if (keys != NULL && i < keys->len) {
      g_autofree char *normalized = normalize_key(g_ptr_array_index(keys, i));
      const gpointer row = normalized != NULL ? g_hash_table_lookup(state->index, normalized) : NULL;

      if (row != NULL) {
        value = state->table->cells[(gsize)(GPOINTER_TO_UINT(row) - 1) * state->table->n_fields + state->columns[which]];
      }
    }
    g_ptr_array_add(column, g_strdup(value));
  }
  return column;
}

GPtrArray *osmium_vector_layer_get_all_fields(OsmiumVectorLayer *self) {
  g_return_val_if_fail(OSMIUM_IS_VECTOR_LAYER(self), NULL);

  GPtrArray *fields = osmium_vector_layer_get_fields(self);

  if (fields == NULL) {
    return NULL;
  }
  for (guint i = 0; i < osmium_join_count_fields(self); i++) {
    OsmiumFeatureFieldType type = OSMIUM_FEATURE_FIELD_STRING;
    const char *name = osmium_join_field_name(self, i, &type);
    OsmiumFeatureField *field = g_new0(OsmiumFeatureField, 1);

    field->name = g_strdup(name);
    field->value = g_strdup("");
    field->type = type;
    field->virtual_field = TRUE;
    g_ptr_array_add(fields, field);
  }
  return fields;
}
