#include "wms.h"
#include "osmium.h"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WMS_MAX_RESPONSE (16u * 1024u * 1024u)

typedef struct {
  char *data;
  size_t len;
} Buffer;

static int buf_append(Buffer *buf, const char *chunk, size_t n) {
  if (n > WMS_MAX_RESPONSE - buf->len) {
    return 0;
  }

  char *grown = realloc(buf->data, buf->len + n + 1);
  if (!grown) {
    return 0;
  }

  memcpy(grown + buf->len, chunk, n);
  buf->len += n;
  grown[buf->len] = '\0';
  buf->data = grown;
  return 1;
}

static size_t buffer_write(char *chunk, size_t size, size_t nmemb, void *userdata) {
  size_t n = size * nmemb;
  return buf_append(userdata, chunk, n) ? n : 0;
}

static int query_add(Buffer *q, const char *key, const char *value) {
  if (!buf_append(q, key, strlen(key)) || !buf_append(q, "=", 1)) {
    return 0;
  }
  for (const char *p = value; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        strchr("-._~,:/", c)) {
      if (!buf_append(q, (const char *)&c, 1)) {
        return 0;
      }
    } else {
      char esc[4];
      snprintf(esc, sizeof(esc), "%%%02X", c);
      if (!buf_append(q, esc, 3)) {
        return 0;
      }
    }
  }
  return buf_append(q, "&", 1);
}

static int query_add_double_list(Buffer *q, const char *key, const double *v, size_t n) {
  locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!c_loc) {
    return 0;
  }
  locale_t old = uselocale(c_loc);

  char text[4 * 40];
  size_t len = 0;
  for (size_t i = 0; i < n && len < sizeof(text); i++) {
    int w = snprintf(text + len, sizeof(text) - len, i ? ",%.10g" : "%.10g", v[i]);
    if (w < 0) {
      len = sizeof(text);
      break;
    }
    len += (size_t)w;
  }

  uselocale(old);
  freelocale(c_loc);
  if (len >= sizeof(text)) {
    return 0;
  }
  return query_add(q, key, text);
}

static char *join_url(const char *base, const Buffer *query) {
  size_t blen = strlen(base);
  size_t qlen = query->len > 0 ? query->len - 1 : 0;
  char *url = malloc(blen + qlen + 2);
  if (!url) {
    return NULL;
  }

  const char *sep = "";
  if (!strchr(base, '?')) {
    sep = "?";
  } else if (blen > 0 && base[blen - 1] != '?' && base[blen - 1] != '&') {
    sep = "&";
  }
  snprintf(url, blen + qlen + 2, "%s%s%.*s", base, sep, (int)qlen, query->data);
  return url;
}

static char *build_url(const char *base) {
  static const char query[] = "SERVICE=WMS&REQUEST=GetCapabilities&";
  Buffer q = {0};
  if (!buf_append(&q, query, sizeof(query) - 1)) {
    free(q.data);
    return NULL;
  }
  char *url = join_url(base, &q);
  free(q.data);
  return url;
}

static osmium_error
fetch(const char *url, const char *user_agent, Buffer *buf, char **content_type) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return OSMIUM_NULL_POINTER;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : "Osmium/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode result = curl_easy_perform(curl);

  long status = 0;
  if (result == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
      result = CURLE_HTTP_RETURNED_ERROR;
    }
  }

  if (result == CURLE_OK && content_type) {
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    *content_type = ct ? strdup(ct) : NULL;
  }

  curl_easy_cleanup(curl);
  return result == CURLE_OK ? OSMIUM_OK : OSMIUM_NETWORK_ERROR;
}

static int is_elem(const xmlNode *node, const char *name) {
  return node->type == XML_ELEMENT_NODE && strcmp((const char *)node->name, name) == 0;
}

static xmlNode *child(xmlNode *parent, const char *name) {
  for (xmlNode *n = parent ? parent->children : NULL; n; n = n->next) {
    if (is_elem(n, name)) {
      return n;
    }
  }
  return NULL;
}

static char *text_of(const xmlNode *node) {
  xmlChar *raw = xmlNodeGetContent(node);
  if (!raw) {
    return NULL;
  }

  const char *start = (const char *)raw;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
    start++;
  }
  size_t len = strlen(start);
  while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\n' ||
                     start[len - 1] == '\r')) {
    len--;
  }

  char *copy = len > 0 ? strndup(start, len) : NULL;
  xmlFree(raw);
  return copy;
}

static char *child_text(xmlNode *parent, const char *name) {
  xmlNode *n = child(parent, name);
  return n ? text_of(n) : NULL;
}

static char *attr_copy(xmlNode *node, const char *name) {
  xmlChar *v = xmlGetProp(node, (const xmlChar *)name);
  if (!v) {
    return NULL;
  }
  char *copy = strdup((const char *)v);
  xmlFree(v);
  return copy;
}

static int parse_double(const char *s, double *out) {
  if (!s) {
    return 0;
  }

  locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!c_loc) {
    return 0;
  }
  locale_t old = uselocale(c_loc);

  char *end = NULL;
  double v = strtod(s, &end);
  int ok = end != s;

  uselocale(old);
  freelocale(c_loc);
  if (ok) {
    *out = v;
  }
  return ok;
}

static int str_push(char ***list, size_t *n, char *value) {
  char **grown = realloc(*list, (*n + 1) * sizeof(char *));
  if (!grown) {
    free(value);
    return 0;
  }
  grown[(*n)++] = value;
  *list = grown;
  return 1;
}

static void str_list_free(char **list, size_t n) {
  for (size_t i = 0; i < n; i++) {
    free(list[i]);
  }
  free(list);
}

static int str_list_has(char **list, size_t n, const char *value) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i], value) == 0) {
      return 1;
    }
  }
  return 0;
}

static void layer_free(OsmiumWmsCapsLayer *layer) {
  if (!layer) {
    return;
  }
  for (size_t i = 0; i < layer->n_children; i++) {
    layer_free(layer->children[i]);
  }
  free(layer->children);
  str_list_free(layer->crs, layer->n_crs);
  free(layer->name);
  free(layer->title);
  free(layer->abstract);
  free(layer);
}

static int layer_read_bbox(OsmiumWmsCapsLayer *layer, xmlNode *node) {
  double v[4];

  xmlNode *geo = child(node, "EX_GeographicBoundingBox");
  if (geo) {
    char *w = child_text(geo, "westBoundLongitude");
    char *s = child_text(geo, "southBoundLatitude");
    char *e = child_text(geo, "eastBoundLongitude");
    char *n = child_text(geo, "northBoundLatitude");
    int ok = parse_double(w, &v[0]) && parse_double(s, &v[1]) && parse_double(e, &v[2]) &&
             parse_double(n, &v[3]);
    free(w);
    free(s);
    free(e);
    free(n);
    if (ok) {
      memcpy(layer->bbox, v, sizeof(v));
      return 1;
    }
  }

  xmlNode *ll = child(node, "LatLonBoundingBox");
  if (ll) {
    char *minx = attr_copy(ll, "minx");
    char *miny = attr_copy(ll, "miny");
    char *maxx = attr_copy(ll, "maxx");
    char *maxy = attr_copy(ll, "maxy");
    int ok = parse_double(minx, &v[0]) && parse_double(miny, &v[1]) && parse_double(maxx, &v[2]) &&
             parse_double(maxy, &v[3]);
    free(minx);
    free(miny);
    free(maxx);
    free(maxy);
    if (ok) {
      memcpy(layer->bbox, v, sizeof(v));
      return 1;
    }
  }

  return 0;
}

static OsmiumWmsCapsLayer *layer_parse(xmlNode *node, const OsmiumWmsCapsLayer *parent) {
  OsmiumWmsCapsLayer *layer = calloc(1, sizeof(*layer));
  if (!layer) {
    return NULL;
  }

  layer->name = child_text(node, "Name");
  layer->title = child_text(node, "Title");
  layer->abstract = child_text(node, "Abstract");

  char *queryable = attr_copy(node, "queryable");
  layer->queryable = queryable ? (strcmp(queryable, "1") == 0 || strcmp(queryable, "true") == 0)
                               : (parent ? parent->queryable : 0);
  free(queryable);

  if (parent) {
    for (size_t i = 0; i < parent->n_crs; i++) {
      char *copy = strdup(parent->crs[i]);
      if (!copy || !str_push(&layer->crs, &layer->n_crs, copy)) {
        goto fail;
      }
    }
    if (parent->has_bbox) {
      layer->has_bbox = 1;
      memcpy(layer->bbox, parent->bbox, sizeof(layer->bbox));
    }
  }

  if (layer_read_bbox(layer, node)) {
    layer->has_bbox = 1;
  }

  for (xmlNode *n = node->children; n; n = n->next) {
    if (is_elem(n, "CRS") || is_elem(n, "SRS")) {
      char *text = text_of(n);
      if (!text) {
        continue;
      }
      for (char *tok = strtok(text, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        if (str_list_has(layer->crs, layer->n_crs, tok)) {
          continue;
        }
        char *copy = strdup(tok);
        if (!copy || !str_push(&layer->crs, &layer->n_crs, copy)) {
          free(text);
          goto fail;
        }
      }
      free(text);
    } else if (is_elem(n, "Layer")) {
      OsmiumWmsCapsLayer *sub = layer_parse(n, layer);
      if (!sub) {
        goto fail;
      }
      OsmiumWmsCapsLayer **grown = realloc(layer->children, (layer->n_children + 1) * sizeof(*grown));
      if (!grown) {
        layer_free(sub);
        goto fail;
      }
      grown[layer->n_children++] = sub;
      layer->children = grown;
    }
  }

  return layer;

fail:
  layer_free(layer);
  return NULL;
}

static osmium_error caps_parse(xmlNode *root, OsmiumWmsCapabilities *caps) {
  caps->version = attr_copy(root, "version");

  xmlNode *service = child(root, "Service");
  caps->service_title = child_text(service, "Title");

  xmlNode *capability = child(root, "Capability");
  if (!capability) {
    return OSMIUM_PARSE_ERROR;
  }

  xmlNode *getmap = child(child(capability, "Request"), "GetMap");
  if (getmap) {
    for (xmlNode *n = getmap->children; n; n = n->next) {
      if (is_elem(n, "Format")) {
        char *fmt = text_of(n);
        if (fmt && !str_push(&caps->formats, &caps->n_formats, fmt)) {
          return OSMIUM_OUT_OF_MEMORY;
        }
      }
    }

    xmlNode *get = child(child(child(getmap, "DCPType"), "HTTP"), "Get");
    xmlNode *res = child(get, "OnlineResource");
    if (res) {
      xmlChar *href = xmlGetNsProp(
          res, (const xmlChar *)"href", (const xmlChar *)"http://www.w3.org/1999/xlink");
      if (!href) {
        href = xmlGetProp(res, (const xmlChar *)"href");
      }
      if (href) {
        caps->getmap_url = strdup((const char *)href);
        xmlFree(href);
      }
    }
  }

  xmlNode *layer = child(capability, "Layer");
  if (layer) {
    caps->root = layer_parse(layer, NULL);
    if (!caps->root) {
      return OSMIUM_OUT_OF_MEMORY;
    }
  }

  return OSMIUM_OK;
}

osmium_error osmium_wms_get_capabilities(const char *server_url,
                                         const char *user_agent,
                                         OsmiumWmsCapabilities **out) {
  if (!server_url || !out) {
    return OSMIUM_NULL_POINTER;
  }
  *out = NULL;

  char *url = build_url(server_url);
  if (!url) {
    return OSMIUM_OUT_OF_MEMORY;
  }

  Buffer buf = {0};
  osmium_error err = fetch(url, user_agent, &buf, NULL);
  free(url);
  if (err != OSMIUM_OK) {
    free(buf.data);
    return err;
  }
  if (!buf.data || buf.len == 0 || buf.len > INT_MAX) {
    free(buf.data);
    return OSMIUM_PARSE_ERROR;
  }

  xmlDoc *doc = xmlReadMemory(buf.data,
                              (int)buf.len,
                              NULL,
                              NULL,
                              XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
  free(buf.data);
  if (!doc) {
    return OSMIUM_PARSE_ERROR;
  }

  xmlNode *root = xmlDocGetRootElement(doc);
  if (!root) {
    err = OSMIUM_PARSE_ERROR;
  } else if (is_elem(root, "ServiceExceptionReport") || is_elem(root, "ExceptionReport")) {
    err = OSMIUM_WMS_EXCEPTION;
  } else if (!is_elem(root, "WMS_Capabilities") && !is_elem(root, "WMT_MS_Capabilities")) {
    err = OSMIUM_PARSE_ERROR;
  } else {
    OsmiumWmsCapabilities *caps = calloc(1, sizeof(*caps));
    if (!caps) {
      err = OSMIUM_OUT_OF_MEMORY;
    } else {
      err = caps_parse(root, caps);
      if (err == OSMIUM_OK) {
        *out = caps;
      } else {
        osmium_wms_capabilities_free(caps);
      }
    }
  }

  xmlFreeDoc(doc);
  return err;
}

void osmium_wms_capabilities_free(OsmiumWmsCapabilities *caps) {
  if (!caps) {
    return;
  }
  free(caps->version);
  free(caps->service_title);
  free(caps->getmap_url);
  str_list_free(caps->formats, caps->n_formats);
  layer_free(caps->root);
  free(caps);
}

static int crs_is_lat_first(const char *crs) {
  static const char *const codes[] = {"EPSG:4326", "EPSG:4258", "EPSG:4269"};
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
    if (strcasecmp(crs, codes[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

osmium_error osmium_wms_get_map(const OsmiumWmsGetMapParams *params,
                                const char *user_agent,
                                OsmiumWmsImage *out) {
  if (!params || !out || !params->url || !params->layers || !params->crs || !params->format) {
    return OSMIUM_NULL_POINTER;
  }
  memset(out, 0, sizeof(*out));

  if (params->width <= 0 || params->height <= 0 || params->bbox[0] >= params->bbox[2] ||
      params->bbox[1] >= params->bbox[3]) {
    return OSMIUM_INVALID_BBOX;
  }

  const char *version = params->version ? params->version : "1.3.0";
  int v13 = strcmp(version, "1.3.0") == 0;

  double bbox[4] = {params->bbox[0], params->bbox[1], params->bbox[2], params->bbox[3]};
  if (v13 && crs_is_lat_first(params->crs)) {
    bbox[0] = params->bbox[1];
    bbox[1] = params->bbox[0];
    bbox[2] = params->bbox[3];
    bbox[3] = params->bbox[2];
  }

  char width[16], height[16];
  snprintf(width, sizeof(width), "%d", params->width);
  snprintf(height, sizeof(height), "%d", params->height);

  Buffer q = {0};
  int ok = query_add(&q, "SERVICE", "WMS") && query_add(&q, "VERSION", version) &&
           query_add(&q, "REQUEST", "GetMap") && query_add(&q, "LAYERS", params->layers) &&
           query_add(&q, "STYLES", params->styles ? params->styles : "") &&
           query_add(&q, v13 ? "CRS" : "SRS", params->crs) &&
           query_add_double_list(&q, "BBOX", bbox, 4) && query_add(&q, "WIDTH", width) &&
           query_add(&q, "HEIGHT", height) && query_add(&q, "FORMAT", params->format) &&
           query_add(&q, "TRANSPARENT", params->transparent ? "TRUE" : "FALSE");
  if (!ok) {
    free(q.data);
    return OSMIUM_OUT_OF_MEMORY;
  }

  char *url = join_url(params->url, &q);
  free(q.data);
  if (!url) {
    return OSMIUM_OUT_OF_MEMORY;
  }

  Buffer body = {0};
  char *content_type = NULL;
  osmium_error err = fetch(url, user_agent, &body, &content_type);
  free(url);

  if (err == OSMIUM_OK) {
    if (!body.data || body.len == 0) {
      err = OSMIUM_PARSE_ERROR;
    } else if (content_type && strstr(content_type, "xml")) {
      err = OSMIUM_WMS_EXCEPTION;
    } else if (!content_type || strncmp(content_type, "image/", 6) != 0) {
      err = OSMIUM_PARSE_ERROR;
    }
  }
  if (err != OSMIUM_OK) {
    free(body.data);
    free(content_type);
    return err;
  }

  out->data = (unsigned char *)body.data;
  out->len = body.len;
  out->content_type = content_type;
  return OSMIUM_OK;
}

void osmium_wms_image_free(OsmiumWmsImage *image) {
  if (!image) {
    return;
  }
  free(image->data);
  free(image->content_type);
  memset(image, 0, sizeof(*image));
}
