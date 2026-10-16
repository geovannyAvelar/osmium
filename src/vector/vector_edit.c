/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vector_edit.h"

#include <stdlib.h>
#include <string.h>

static char *dup_or_null(const char *text) {
  return text ? strdup(text) : NULL;
}

static void command_clear(OsmiumEditCommand *command) {
  osmium_geometry_clear(&command->before);
  osmium_geometry_clear(&command->after);
  free(command->field);
  free(command->old_value);
  free(command->new_value);
  free(command->previous_override);
  memset(command, 0, sizeof(*command));
}

OsmiumEditSession *osmium_edit_session_new(OsmiumVectorData *data) {
  OsmiumEditSession *session = calloc(1, sizeof(*session));

  if (session) {
    session->data = osmium_vector_data_ref(data);
    session->next_group = 1;
  }
  return session;
}

static void clear_history(OsmiumEditSession *session) {
  for (size_t i = 0; i < session->n_commands; i++) {
    command_clear(&session->commands[i]);
  }
  session->n_commands = 0;
  session->cursor = 0;

  for (size_t i = 0; i < session->n_overrides; i++) {
    free(session->overrides[i].field);
    free(session->overrides[i].value);
  }
  session->n_overrides = 0;
}

void osmium_edit_session_clear(OsmiumEditSession *session) {
  clear_history(session);
}

void osmium_edit_session_free(OsmiumEditSession *session) {
  if (!session) {
    return;
  }
  clear_history(session);
  free(session->commands);
  free(session->overrides);
  osmium_vector_data_free(session->data);
  free(session);
}

int osmium_edit_session_is_dirty(const OsmiumEditSession *session) {
  return session->cursor > 0;
}

int osmium_edit_session_can_undo(const OsmiumEditSession *session) {
  return session->cursor > 0;
}

int osmium_edit_session_can_redo(const OsmiumEditSession *session) {
  return session->cursor < session->n_commands;
}

void osmium_edit_session_begin_group(OsmiumEditSession *session) {
  if (session->open_group == 0) {
    session->open_group = session->next_group++;
  }
}

void osmium_edit_session_end_group(OsmiumEditSession *session) {
  session->open_group = 0;
}

static OsmiumAttributeOverride *find_override(const OsmiumEditSession *session, long long fid, const char *field) {
  for (size_t i = 0; i < session->n_overrides; i++) {
    if (session->overrides[i].fid == fid && strcmp(session->overrides[i].field, field) == 0) {
      return &session->overrides[i];
    }
  }
  return NULL;
}

const char *osmium_edit_get_override(const OsmiumEditSession *session, long long fid, const char *field) {
  const OsmiumAttributeOverride *entry = find_override(session, fid, field);

  return entry ? entry->value : NULL;
}

static void set_override(OsmiumEditSession *session, long long fid, const char *field, const char *value) {
  OsmiumAttributeOverride *entry = find_override(session, fid, field);

  if (!value) {
    if (entry) {
      free(entry->field);
      free(entry->value);
      *entry = session->overrides[--session->n_overrides];
    }
    return;
  }

  if (entry) {
    free(entry->value);
    entry->value = strdup(value);
    return;
  }

  if (session->n_overrides == session->cap_overrides) {
    size_t cap = session->cap_overrides ? session->cap_overrides * 2 : 16;
    OsmiumAttributeOverride *grown = realloc(session->overrides, cap * sizeof(*grown));

    if (!grown) {
      return;
    }
    session->overrides = grown;
    session->cap_overrides = cap;
  }

  entry = &session->overrides[session->n_overrides++];
  entry->fid = fid;
  entry->field = strdup(field);
  entry->value = strdup(value);
}

static void apply(OsmiumEditSession *session, OsmiumEditCommand *command) {
  OsmiumVectorData *data = session->data;

  switch (command->type) {
  case OSMIUM_EDIT_ADD:
    osmium_vector_data_set_deleted(data, command->index, 0);
    break;
  case OSMIUM_EDIT_DELETE:
    osmium_vector_data_set_deleted(data, command->index, 1);
    break;
  case OSMIUM_EDIT_GEOMETRY:
    osmium_vector_data_set_geometry(data, command->index, &command->after);
    if (command->previous_state == OSMIUM_FEATURE_CLEAN) {
      osmium_vector_data_set_state(data, command->index, OSMIUM_FEATURE_MODIFIED);
    }
    break;
  case OSMIUM_EDIT_ATTRIBUTE:
    set_override(session, command->fid, command->field, command->new_value);
    break;
  }
}

static void revert(OsmiumEditSession *session, OsmiumEditCommand *command) {
  OsmiumVectorData *data = session->data;

  switch (command->type) {
  case OSMIUM_EDIT_ADD:
    osmium_vector_data_set_deleted(data, command->index, 1);
    break;
  case OSMIUM_EDIT_DELETE:
    osmium_vector_data_set_deleted(data, command->index, 0);
    break;
  case OSMIUM_EDIT_GEOMETRY:
    osmium_vector_data_set_geometry(data, command->index, &command->before);
    osmium_vector_data_set_state(data, command->index, command->previous_state);
    break;
  case OSMIUM_EDIT_ATTRIBUTE:
    set_override(session, command->fid, command->field, command->override_existed ? command->previous_override : NULL);
    break;
  }
}

/* Drops the redo history and returns room for one more command. */
static OsmiumEditCommand *push(OsmiumEditSession *session) {
  for (size_t i = session->cursor; i < session->n_commands; i++) {
    command_clear(&session->commands[i]);
  }
  session->n_commands = session->cursor;

  if (session->n_commands == session->cap_commands) {
    size_t cap = session->cap_commands ? session->cap_commands * 2 : 32;
    OsmiumEditCommand *grown = realloc(session->commands, cap * sizeof(*grown));

    if (!grown) {
      return NULL;
    }
    session->commands = grown;
    session->cap_commands = cap;
  }

  OsmiumEditCommand *command = &session->commands[session->n_commands];

  memset(command, 0, sizeof(*command));
  command->group = session->open_group;
  return command;
}

static void commit(OsmiumEditSession *session) {
  session->n_commands++;
  session->cursor = session->n_commands;
}

long long osmium_edit_add_feature(OsmiumEditSession *session, const OsmiumGeometry *geometry) {
  OsmiumEditCommand *command = push(session);
  long long fid = osmium_vector_data_new_fid(session->data);
  ptrdiff_t index;

  if (!command) {
    return 0;
  }

  index = osmium_vector_data_append_feature(session->data, fid, geometry);
  if (index < 0) {
    return 0;
  }

  command->type = OSMIUM_EDIT_ADD;
  command->fid = fid;
  command->index = (size_t)index;
  commit(session);
  return fid;
}

int osmium_edit_set_geometry(OsmiumEditSession *session, long long fid, const OsmiumGeometry *geometry) {
  ptrdiff_t index = osmium_vector_data_find_fid(session->data, fid);
  OsmiumEditCommand *command;

  if (index < 0 || !(command = push(session))) {
    return 0;
  }

  command->type = OSMIUM_EDIT_GEOMETRY;
  command->fid = fid;
  command->index = (size_t)index;
  command->previous_state = session->data->features[index].state;

  if (!osmium_vector_data_copy_geometry(session->data, (size_t)index, &command->before) ||
      !osmium_geometry_copy(geometry, &command->after)) {
    command_clear(command);
    return 0;
  }

  apply(session, command);
  commit(session);
  return 1;
}

int osmium_edit_delete_feature(OsmiumEditSession *session, long long fid) {
  ptrdiff_t index = osmium_vector_data_find_fid(session->data, fid);
  OsmiumEditCommand *command;

  if (index < 0 || session->data->features[index].deleted || !(command = push(session))) {
    return 0;
  }

  command->type = OSMIUM_EDIT_DELETE;
  command->fid = fid;
  command->index = (size_t)index;
  apply(session, command);
  commit(session);
  return 1;
}

int osmium_edit_set_attribute(OsmiumEditSession *session,
                              long long fid,
                              const char *field,
                              const char *old_value,
                              const char *new_value) {
  OsmiumEditCommand *command = push(session);
  const OsmiumAttributeOverride *existing;

  if (!command) {
    return 0;
  }

  existing = find_override(session, fid, field);
  command->type = OSMIUM_EDIT_ATTRIBUTE;
  command->fid = fid;
  command->field = strdup(field);
  command->old_value = dup_or_null(old_value);
  command->new_value = dup_or_null(new_value);
  command->override_existed = existing != NULL;
  command->previous_override = existing ? strdup(existing->value) : NULL;

  apply(session, command);
  commit(session);
  return 1;
}

int osmium_edit_undo(OsmiumEditSession *session) {
  if (session->cursor == 0) {
    return 0;
  }

  const unsigned group = session->commands[session->cursor - 1].group;

  do {
    revert(session, &session->commands[--session->cursor]);
  } while (group != 0 && session->cursor > 0 && session->commands[session->cursor - 1].group == group);

  return 1;
}

int osmium_edit_redo(OsmiumEditSession *session) {
  if (session->cursor >= session->n_commands) {
    return 0;
  }

  const unsigned group = session->commands[session->cursor].group;

  do {
    apply(session, &session->commands[session->cursor++]);
  } while (group != 0 && session->cursor < session->n_commands && session->commands[session->cursor].group == group);

  return 1;
}

void osmium_edit_discard(OsmiumEditSession *session) {
  while (osmium_edit_undo(session)) {
  }
  clear_history(session);
}
