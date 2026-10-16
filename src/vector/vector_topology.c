/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vector_edit.h"

#include <cpl_error.h>
#include <gdal.h>
#include <math.h>
#include <ogr_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OSMIUM_TOPOLOGY_MAX_ISSUES 5000

typedef struct {
  OsmiumTopologyIssue *items;
  size_t count;
  size_t capacity;
} IssueList;

typedef struct {
  OGRGeometryH geometry;
  const OsmiumVectorFeature *feature;
  int valid;
  double min_x, min_y, max_x, max_y;
} Candidate;

static char captured_message[256];

static void CPL_STDCALL capture_handler(CPLErr class_, CPLErrorNum number, const char *message) {
  snprintf(captured_message, sizeof(captured_message), "%s", message ? message : "");
}

static void add_issue(IssueList *list, OsmiumTopologyKind kind, long long fid, int has_other, long long other,
                      double lon, double lat, const char *message) {
  if (list->count >= OSMIUM_TOPOLOGY_MAX_ISSUES) {
    return;
  }
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    OsmiumTopologyIssue *grown = realloc(list->items, capacity * sizeof(*grown));

    if (!grown) {
      return;
    }
    list->items = grown;
    list->capacity = capacity;
  }

  OsmiumTopologyIssue *issue = &list->items[list->count++];

  memset(issue, 0, sizeof(*issue));
  issue->kind = kind;
  issue->fid = fid;
  issue->has_other = has_other;
  issue->other_fid = other;
  issue->lon = lon;
  issue->lat = lat;
  snprintf(issue->message, sizeof(issue->message), "%s", message);
}

static void centroid_of(OGRGeometryH geometry, double *lon, double *lat) {
  OGRGeometryH centroid = OGR_G_CreateGeometry(wkbPoint);

  if (OGR_G_Centroid(geometry, centroid) == OGRERR_NONE) {
    *lon = OGR_G_GetX(centroid, 0);
    *lat = OGR_G_GetY(centroid, 0);
  } else {
    OGREnvelope envelope;

    OGR_G_GetEnvelope(geometry, &envelope);
    *lon = (envelope.MinX + envelope.MaxX) / 2.0;
    *lat = (envelope.MinY + envelope.MaxY) / 2.0;
  }
  OGR_G_DestroyGeometry(centroid);
}

static int has_duplicate_vertices(const OsmiumVectorFeature *f) {
  for (size_t p = 0; p < f->n_parts; p++) {
    for (size_t v = f->part_start[p] + 1; v < f->part_start[p + 1]; v++) {
      if (f->ll[v * 2] == f->ll[(v - 1) * 2] && f->ll[v * 2 + 1] == f->ll[(v - 1) * 2 + 1]) {
        return 1;
      }
    }
  }
  return 0;
}

static int by_min_x(const void *a, const void *b) {
  const Candidate *left = *(const Candidate *const *)a;
  const Candidate *right = *(const Candidate *const *)b;

  return left->min_x < right->min_x ? -1 : (left->min_x > right->min_x ? 1 : 0);
}

osmium_error osmium_vector_data_check_topology(OsmiumVectorData *vec, OsmiumTopologyIssue **issues, size_t *n_issues) {
  IssueList list = {0};
  Candidate *candidates = calloc(vec->n_features ? vec->n_features : 1, sizeof(*candidates));
  Candidate **sorted = calloc(vec->n_features ? vec->n_features : 1, sizeof(*sorted));
  size_t n = 0;

  if (!candidates || !sorted) {
    free(candidates);
    free(sorted);
    return OSMIUM_OUT_OF_MEMORY;
  }

  *issues = NULL;
  *n_issues = 0;

  CPLPushErrorHandler(capture_handler);

  for (size_t i = 0; i < vec->n_features; i++) {
    const OsmiumVectorFeature *f = &vec->features[i];
    Candidate *c = &candidates[n];

    if (f->deleted) {
      continue;
    }

    c->feature = f;
    c->geometry = osmium_vector_build_geometry(vec, f, NULL);
    if (!c->geometry) {
      continue;
    }

    OGREnvelope envelope;

    OGR_G_GetEnvelope(c->geometry, &envelope);
    c->min_x = envelope.MinX;
    c->min_y = envelope.MinY;
    c->max_x = envelope.MaxX;
    c->max_y = envelope.MaxY;

    double lon, lat;

    if (has_duplicate_vertices(f)) {
      centroid_of(c->geometry, &lon, &lat);
      add_issue(&list, OSMIUM_TOPOLOGY_DEGENERATE, f->fid, 0, 0, lon, lat, "Repeated vertices (zero-length segment)");
    }

    captured_message[0] = '\0';
    if (f->kind == OSMIUM_VECTOR_POLYGON) {
      c->valid = OGR_G_IsValid(c->geometry);
      if (!c->valid) {
        double x, y;

        centroid_of(c->geometry, &lon, &lat);
        if (sscanf(strstr(captured_message, "point ") ? strstr(captured_message, "point ") + 6 : "", "%lf %lf", &x, &y) == 2) {
          lon = x;
          lat = y;
        }
        add_issue(&list, OSMIUM_TOPOLOGY_INVALID, f->fid, 0, 0, lon, lat,
                  captured_message[0] ? captured_message : "Invalid polygon");
      }
    } else if (f->kind == OSMIUM_VECTOR_LINE) {
      c->valid = 1;
      if (!OGR_G_IsSimple(c->geometry)) {
        centroid_of(c->geometry, &lon, &lat);
        add_issue(&list, OSMIUM_TOPOLOGY_SELF_INTERSECTION, f->fid, 0, 0, lon, lat, "The line crosses or touches itself");
      }
    } else {
      c->valid = 1;
    }

    sorted[n] = c;
    n++;
  }

  qsort(sorted, n, sizeof(*sorted), by_min_x);

  for (size_t i = 0; i < n; i++) {
    Candidate *a = sorted[i];

    for (size_t j = i + 1; j < n && sorted[j]->min_x <= a->max_x; j++) {
      Candidate *b = sorted[j];

      if (b->max_y < a->min_y || b->min_y > a->max_y || !a->valid || !b->valid) {
        continue;
      }

      const int same_box = a->min_x == b->min_x && a->min_y == b->min_y && a->max_x == b->max_x && a->max_y == b->max_y;

      if (same_box && OGR_G_Equals(a->geometry, b->geometry)) {
        double lon, lat;

        centroid_of(a->geometry, &lon, &lat);
        add_issue(&list, OSMIUM_TOPOLOGY_DUPLICATE, a->feature->fid, 1, b->feature->fid, lon, lat, "Identical geometry");
        continue;
      }

      if (a->feature->kind != OSMIUM_VECTOR_POLYGON || b->feature->kind != OSMIUM_VECTOR_POLYGON) {
        continue;
      }

      if (OGR_G_Overlaps(a->geometry, b->geometry) || OGR_G_Contains(a->geometry, b->geometry) ||
          OGR_G_Contains(b->geometry, a->geometry)) {
        OGRGeometryH common = OGR_G_Intersection(a->geometry, b->geometry);

        if (common) {
          const double area = OGR_G_Area(common);
          const double smaller = fmin(OGR_G_Area(a->geometry), OGR_G_Area(b->geometry));

          if (area > 0.0 && area > smaller * 1e-6) {
            double lon, lat;

            centroid_of(common, &lon, &lat);
            add_issue(&list, OSMIUM_TOPOLOGY_OVERLAP, a->feature->fid, 1, b->feature->fid, lon, lat,
                      "The polygons overlap");
          }
          OGR_G_DestroyGeometry(common);
        }
      }
    }
  }

  CPLPopErrorHandler();

  for (size_t i = 0; i < n; i++) {
    OGR_G_DestroyGeometry(candidates[i].geometry);
  }
  free(candidates);
  free(sorted);

  *issues = list.items;
  *n_issues = list.count;
  return OSMIUM_OK;
}
