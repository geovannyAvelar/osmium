/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "vector_data.h"

typedef enum {
  OSMIUM_EDIT_ADD,
  OSMIUM_EDIT_DELETE,
  OSMIUM_EDIT_GEOMETRY,
  OSMIUM_EDIT_ATTRIBUTE
} OsmiumEditType;

typedef struct {
  OsmiumEditType type;
  unsigned group;
  long long fid;
  size_t index;
  int previous_state;
  OsmiumGeometry before;
  OsmiumGeometry after;
  char *field;
  char *old_value;
  char *new_value;
  int override_existed;
  char *previous_override;
} OsmiumEditCommand;

typedef struct {
  long long fid;
  char *field;
  char *value;
} OsmiumAttributeOverride;

typedef struct OsmiumEditSession {
  OsmiumVectorData *data;
  OsmiumEditCommand *commands;
  size_t n_commands;
  size_t cap_commands;
  size_t cursor;
  unsigned next_group;
  unsigned open_group;
  OsmiumAttributeOverride *overrides;
  size_t n_overrides;
  size_t cap_overrides;
} OsmiumEditSession;

OsmiumEditSession *osmium_edit_session_new(OsmiumVectorData *data);
void osmium_edit_session_free(OsmiumEditSession *session);

int osmium_edit_session_is_dirty(const OsmiumEditSession *session);
int osmium_edit_session_can_undo(const OsmiumEditSession *session);
int osmium_edit_session_can_redo(const OsmiumEditSession *session);

/* Commands pushed between begin_group and end_group undo and redo together. */
void osmium_edit_session_begin_group(OsmiumEditSession *session);
void osmium_edit_session_end_group(OsmiumEditSession *session);

/* Returns the temporary (negative) feature id of the new feature, or 0 on failure. */
long long osmium_edit_add_feature(OsmiumEditSession *session, const OsmiumGeometry *geometry);
int osmium_edit_set_geometry(OsmiumEditSession *session, long long fid, const OsmiumGeometry *geometry);
int osmium_edit_delete_feature(OsmiumEditSession *session, long long fid);
int osmium_edit_set_attribute(OsmiumEditSession *session,
                              long long fid,
                              const char *field,
                              const char *old_value,
                              const char *new_value);

/* The edited value of a field, or NULL when the feature's field was not changed. */
const char *osmium_edit_get_override(const OsmiumEditSession *session, long long fid, const char *field);

int osmium_edit_undo(OsmiumEditSession *session);
int osmium_edit_redo(OsmiumEditSession *session);

/* Forgets the history without touching the data. */
void osmium_edit_session_clear(OsmiumEditSession *session);

/* Reverts every change in memory and forgets the history. */
void osmium_edit_discard(OsmiumEditSession *session);

/* Writes all changes into the source file. On success the history is cleared and the features of
 * the session data are stale: the caller reloads them. */
osmium_error osmium_edit_save(OsmiumEditSession *session, char *message, size_t message_size);

/* Builds an OGR geometry (as an opaque pointer) from a feature's lon/lat vertices, optionally
 * transformed. The caller owns it (OGR_G_DestroyGeometry). */
void *osmium_vector_build_geometry(const OsmiumVectorData *vec, const OsmiumVectorFeature *f, OsmiumTransform *transform);

typedef enum {
  OSMIUM_TOPOLOGY_INVALID,
  OSMIUM_TOPOLOGY_SELF_INTERSECTION,
  OSMIUM_TOPOLOGY_OVERLAP,
  OSMIUM_TOPOLOGY_DUPLICATE,
  OSMIUM_TOPOLOGY_DEGENERATE
} OsmiumTopologyKind;

typedef struct {
  OsmiumTopologyKind kind;
  long long fid;
  long long other_fid; /* -1 unless the problem involves two features (-2 for none, ids can be negative) */
  int has_other;
  double lon;
  double lat;
  char message[128];
} OsmiumTopologyIssue;

/* Checks the features in memory (including unsaved edits). Issues are allocated; free with free(). */
osmium_error osmium_vector_data_check_topology(OsmiumVectorData *vec, OsmiumTopologyIssue **issues, size_t *n_issues);
