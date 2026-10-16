/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "osmium_expression.h"

#include <math.h>
#include <string.h>

typedef enum { V_NULL, V_NUM, V_STR, V_BOOL } ValueKind;

typedef struct {
  ValueKind kind;
  double number;
  char *text;
  gboolean owned;
} Value;

typedef enum {
  N_NUMBER,
  N_STRING,
  N_BOOL,
  N_NULL,
  N_FIELD,
  N_NEG,
  N_NOT,
  N_ADD,
  N_SUB,
  N_MUL,
  N_DIV,
  N_MOD,
  N_CONCAT,
  N_EQ,
  N_NE,
  N_LT,
  N_LE,
  N_GT,
  N_GE,
  N_AND,
  N_OR,
  N_LIKE,
  N_ILIKE,
  N_IN,
  N_IS_NULL,
  N_IS_NOT_NULL,
  N_BETWEEN,
  N_FUNC
} NodeKind;

typedef struct Node Node;

struct Node {
  NodeKind kind;
  double number;
  char *text;
  guint field;
  GPtrArray *children;
};

struct _OsmiumExpression {
  Node *root;
};

typedef enum { T_END, T_NUMBER, T_STRING, T_IDENT, T_QIDENT, T_OP, T_LPAREN, T_RPAREN, T_COMMA } TokenKind;

typedef struct {
  TokenKind kind;
  char *text;
  double number;
} Token;

typedef struct {
  const char *input;
  const char *pos;
  Token token;
  const char *const *fields;
  guint n_fields;
  GError **error;
  gboolean failed;
} Parser;

static void node_free(Node *node) {
  if (node == NULL) {
    return;
  }
  g_free(node->text);
  if (node->children != NULL) {
    g_ptr_array_unref(node->children);
  }
  g_free(node);
}

static Node *node_new(NodeKind kind) {
  Node *node = g_new0(Node, 1);

  node->kind = kind;
  node->children = g_ptr_array_new_with_free_func((GDestroyNotify)node_free);
  return node;
}

static void fail(Parser *p, const char *format, const char *detail) {
  if (!p->failed && p->error != NULL) {
    g_autofree char *message = g_strdup_printf(format, detail != NULL ? detail : "");

    g_set_error_literal(p->error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
  }
  p->failed = TRUE;
}

static void token_clear(Token *token) {
  g_clear_pointer(&token->text, g_free);
}

static void advance(Parser *p) {
  token_clear(&p->token);
  while (g_ascii_isspace(*p->pos)) {
    p->pos++;
  }

  const char c = *p->pos;

  if (c == '\0') {
    p->token.kind = T_END;
  } else if (g_ascii_isdigit(c) || (c == '.' && g_ascii_isdigit(p->pos[1]))) {
    char *end;

    p->token.kind = T_NUMBER;
    p->token.number = g_ascii_strtod(p->pos, &end);
    p->pos = end;
  } else if (c == '\'') {
    GString *text = g_string_new(NULL);

    p->pos++;
    while (*p->pos != '\0') {
      if (*p->pos == '\'' && p->pos[1] == '\'') {
        g_string_append_c(text, '\'');
        p->pos += 2;
      } else if (*p->pos == '\'') {
        break;
      } else {
        g_string_append_c(text, *p->pos++);
      }
    }
    if (*p->pos != '\'') {
      fail(p, "Unterminated text", NULL);
    } else {
      p->pos++;
    }
    p->token.kind = T_STRING;
    p->token.text = g_string_free(text, FALSE);
  } else if (c == '"') {
    GString *text = g_string_new(NULL);

    p->pos++;
    while (*p->pos != '\0' && *p->pos != '"') {
      g_string_append_c(text, *p->pos++);
    }
    if (*p->pos != '"') {
      fail(p, "Unterminated field name", NULL);
    } else {
      p->pos++;
    }
    p->token.kind = T_QIDENT;
    p->token.text = g_string_free(text, FALSE);
  } else if (g_ascii_isalpha(c) || c == '_' || (guchar)c >= 0x80) {
    const char *start = p->pos;

    while (g_ascii_isalnum(*p->pos) || *p->pos == '_' || (guchar)*p->pos >= 0x80) {
      p->pos++;
    }
    p->token.kind = T_IDENT;
    p->token.text = g_strndup(start, (gsize)(p->pos - start));
  } else if (c == '(') {
    p->token.kind = T_LPAREN;
    p->pos++;
  } else if (c == ')') {
    p->token.kind = T_RPAREN;
    p->pos++;
  } else if (c == ',') {
    p->token.kind = T_COMMA;
    p->pos++;
  } else {
    static const char *const operators[] = {"<>", "!=", "<=", ">=", "==", "||", "=", "<", ">", "+", "-", "*", "/", "%", NULL};

    for (guint i = 0; operators[i] != NULL; i++) {
      const gsize n = strlen(operators[i]);

      if (strncmp(p->pos, operators[i], n) == 0) {
        p->token.kind = T_OP;
        p->token.text = g_strdup(operators[i]);
        p->pos += n;
        return;
      }
    }
    {
      char bad[2] = {c, '\0'};

      fail(p, "Unexpected character %s", bad);
    }
    p->token.kind = T_END;
  }
}

static gboolean is_keyword(const Parser *p, const char *word) {
  return p->token.kind == T_IDENT && g_ascii_strcasecmp(p->token.text, word) == 0;
}

static gboolean is_op(const Parser *p, const char *op) {
  return p->token.kind == T_OP && strcmp(p->token.text, op) == 0;
}

static Node *parse_or(Parser *p);

static Node *binary(NodeKind kind, Node *left, Node *right) {
  Node *node = node_new(kind);

  g_ptr_array_add(node->children, left);
  g_ptr_array_add(node->children, right);
  return node;
}

static gboolean find_field(Parser *p, const char *name, guint *index) {
  for (guint i = 0; i < p->n_fields; i++) {
    if (g_ascii_strcasecmp(p->fields[i], name) == 0) {
      *index = i;
      return TRUE;
    }
  }
  return FALSE;
}

static Node *parse_primary(Parser *p) {
  Node *node = NULL;

  if (p->failed) {
    return NULL;
  }

  if (p->token.kind == T_NUMBER) {
    node = node_new(N_NUMBER);
    node->number = p->token.number;
    advance(p);
  } else if (p->token.kind == T_STRING) {
    node = node_new(N_STRING);
    node->text = g_strdup(p->token.text);
    advance(p);
  } else if (p->token.kind == T_QIDENT) {
    node = node_new(N_FIELD);
    if (!find_field(p, p->token.text, &node->field)) {
      fail(p, "Unknown field \"%s\"", p->token.text);
    }
    advance(p);
  } else if (p->token.kind == T_IDENT) {
    g_autofree char *name = g_strdup(p->token.text);

    advance(p);
    if (g_ascii_strcasecmp(name, "true") == 0 || g_ascii_strcasecmp(name, "false") == 0) {
      node = node_new(N_BOOL);
      node->number = g_ascii_strcasecmp(name, "true") == 0;
    } else if (g_ascii_strcasecmp(name, "null") == 0) {
      node = node_new(N_NULL);
    } else if (p->token.kind == T_LPAREN) {
      node = node_new(N_FUNC);
      node->text = g_ascii_strdown(name, -1);
      advance(p);
      while (!p->failed && p->token.kind != T_RPAREN) {
        g_ptr_array_add(node->children, parse_or(p));
        if (p->token.kind == T_COMMA) {
          advance(p);
        } else if (p->token.kind != T_RPAREN) {
          fail(p, "Expected ) or , in the call to %s", name);
        }
      }
      if (p->token.kind == T_RPAREN) {
        advance(p);
      }
    } else {
      node = node_new(N_FIELD);
      if (!find_field(p, name, &node->field)) {
        fail(p, "Unknown field \"%s\"", name);
      }
    }
  } else if (p->token.kind == T_LPAREN) {
    advance(p);
    node = parse_or(p);
    if (p->token.kind != T_RPAREN) {
      fail(p, "Expected )", NULL);
    } else {
      advance(p);
    }
  } else {
    fail(p, "Unexpected end of the expression", NULL);
  }
  return node;
}

static Node *parse_unary(Parser *p) {
  if (is_op(p, "-")) {
    Node *node = node_new(N_NEG);

    advance(p);
    g_ptr_array_add(node->children, parse_unary(p));
    return node;
  }
  if (is_op(p, "+")) {
    advance(p);
    return parse_unary(p);
  }
  return parse_primary(p);
}

static Node *parse_multiplicative(Parser *p) {
  Node *left = parse_unary(p);

  while (!p->failed && p->token.kind == T_OP && (is_op(p, "*") || is_op(p, "/") || is_op(p, "%"))) {
    const NodeKind kind = is_op(p, "*") ? N_MUL : (is_op(p, "/") ? N_DIV : N_MOD);

    advance(p);
    left = binary(kind, left, parse_unary(p));
  }
  return left;
}

static Node *parse_additive(Parser *p) {
  Node *left = parse_multiplicative(p);

  while (!p->failed && p->token.kind == T_OP && (is_op(p, "+") || is_op(p, "-") || is_op(p, "||"))) {
    const NodeKind kind = is_op(p, "+") ? N_ADD : (is_op(p, "-") ? N_SUB : N_CONCAT);

    advance(p);
    left = binary(kind, left, parse_multiplicative(p));
  }
  return left;
}

static Node *parse_comparison(Parser *p) {
  Node *left = parse_additive(p);

  while (!p->failed) {
    if (p->token.kind == T_OP) {
      NodeKind kind;

      if (is_op(p, "=") || is_op(p, "==")) {
        kind = N_EQ;
      } else if (is_op(p, "!=") || is_op(p, "<>")) {
        kind = N_NE;
      } else if (is_op(p, "<")) {
        kind = N_LT;
      } else if (is_op(p, "<=")) {
        kind = N_LE;
      } else if (is_op(p, ">")) {
        kind = N_GT;
      } else if (is_op(p, ">=")) {
        kind = N_GE;
      } else {
        break;
      }
      advance(p);
      left = binary(kind, left, parse_additive(p));
    } else if (is_keyword(p, "like") || is_keyword(p, "ilike")) {
      const NodeKind kind = is_keyword(p, "like") ? N_LIKE : N_ILIKE;

      advance(p);
      left = binary(kind, left, parse_additive(p));
    } else if (is_keyword(p, "not")) {
      advance(p);
      if (is_keyword(p, "like") || is_keyword(p, "ilike") || is_keyword(p, "in") || is_keyword(p, "between")) {
        Node *inner = node_new(N_NOT);
        const gboolean in = is_keyword(p, "in");
        const gboolean between = is_keyword(p, "between");
        const NodeKind kind = in ? N_IN : (between ? N_BETWEEN : (is_keyword(p, "like") ? N_LIKE : N_ILIKE));

        advance(p);
        if (in) {
          Node *list = node_new(N_IN);

          g_ptr_array_add(list->children, left);
          if (p->token.kind != T_LPAREN) {
            fail(p, "Expected ( after IN", NULL);
          } else {
            advance(p);
            while (!p->failed && p->token.kind != T_RPAREN) {
              g_ptr_array_add(list->children, parse_additive(p));
              if (p->token.kind == T_COMMA) {
                advance(p);
              }
            }
            advance(p);
          }
          g_ptr_array_add(inner->children, list);
        } else if (between) {
          Node *range = node_new(N_BETWEEN);

          g_ptr_array_add(range->children, left);
          g_ptr_array_add(range->children, parse_additive(p));
          if (!is_keyword(p, "and")) {
            fail(p, "Expected AND in BETWEEN", NULL);
          } else {
            advance(p);
          }
          g_ptr_array_add(range->children, parse_additive(p));
          g_ptr_array_add(inner->children, range);
        } else {
          g_ptr_array_add(inner->children, binary(kind, left, parse_additive(p)));
        }
        left = inner;
      } else {
        fail(p, "Unexpected NOT", NULL);
      }
    } else if (is_keyword(p, "in")) {
      Node *list = node_new(N_IN);

      advance(p);
      g_ptr_array_add(list->children, left);
      if (p->token.kind != T_LPAREN) {
        fail(p, "Expected ( after IN", NULL);
      } else {
        advance(p);
        while (!p->failed && p->token.kind != T_RPAREN) {
          g_ptr_array_add(list->children, parse_additive(p));
          if (p->token.kind == T_COMMA) {
            advance(p);
          } else if (p->token.kind != T_RPAREN) {
            fail(p, "Expected ) in the IN list", NULL);
          }
        }
        advance(p);
      }
      left = list;
    } else if (is_keyword(p, "between")) {
      Node *range = node_new(N_BETWEEN);

      advance(p);
      g_ptr_array_add(range->children, left);
      g_ptr_array_add(range->children, parse_additive(p));
      if (!is_keyword(p, "and")) {
        fail(p, "Expected AND in BETWEEN", NULL);
      } else {
        advance(p);
      }
      g_ptr_array_add(range->children, parse_additive(p));
      left = range;
    } else if (is_keyword(p, "is")) {
      Node *test;

      advance(p);
      if (is_keyword(p, "not")) {
        test = node_new(N_IS_NOT_NULL);
        advance(p);
      } else {
        test = node_new(N_IS_NULL);
      }
      if (!is_keyword(p, "null")) {
        fail(p, "Expected NULL after IS", NULL);
      } else {
        advance(p);
      }
      g_ptr_array_add(test->children, left);
      left = test;
    } else {
      break;
    }
  }
  return left;
}

static Node *parse_not(Parser *p) {
  if (is_keyword(p, "not")) {
    Node *node = node_new(N_NOT);

    advance(p);
    g_ptr_array_add(node->children, parse_not(p));
    return node;
  }
  return parse_comparison(p);
}

static Node *parse_and(Parser *p) {
  Node *left = parse_not(p);

  while (!p->failed && is_keyword(p, "and")) {
    advance(p);
    left = binary(N_AND, left, parse_not(p));
  }
  return left;
}

static Node *parse_or(Parser *p) {
  Node *left = parse_and(p);

  while (!p->failed && is_keyword(p, "or")) {
    advance(p);
    left = binary(N_OR, left, parse_and(p));
  }
  return left;
}

OsmiumExpression *osmium_expression_parse(const char *text, const char *const *field_names, guint n_fields, GError **error) {
  Parser p = {.input = text, .pos = text, .fields = field_names, .n_fields = n_fields, .error = error};
  Node *root;

  advance(&p);
  root = parse_or(&p);
  if (!p.failed && p.token.kind != T_END) {
    fail(&p, "Unexpected text near \"%s\"", p.pos);
  }
  token_clear(&p.token);

  if (p.failed || root == NULL) {
    node_free(root);
    if (error != NULL && *error == NULL) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Empty expression");
    }
    return NULL;
  }

  OsmiumExpression *expression = g_new0(OsmiumExpression, 1);

  expression->root = root;
  return expression;
}

void osmium_expression_free(OsmiumExpression *expression) {
  if (expression == NULL) {
    return;
  }
  node_free(expression->root);
  g_free(expression);
}

static Value null_value(void) {
  Value v = {.kind = V_NULL};

  return v;
}

static Value number_value(double n) {
  Value v = {.kind = V_NUM, .number = n};

  return v;
}

static Value bool_value(gboolean b) {
  Value v = {.kind = V_BOOL, .number = b ? 1.0 : 0.0};

  return v;
}

static Value text_value(char *text) {
  Value v = {.kind = V_STR, .text = text, .owned = TRUE};

  return v;
}

static void value_clear(Value *v) {
  if (v->owned) {
    g_free(v->text);
  }
  v->text = NULL;
  v->owned = FALSE;
}

static gboolean to_number(const Value *v, double *out) {
  char *end;

  switch (v->kind) {
  case V_NUM:
  case V_BOOL:
    *out = v->number;
    return TRUE;
  case V_STR:
    if (v->text[0] == '\0') {
      return FALSE;
    }
    *out = g_ascii_strtod(v->text, &end);
    while (g_ascii_isspace(*end)) {
      end++;
    }
    return *end == '\0' && isfinite(*out);
  case V_NULL:
  default:
    return FALSE;
  }
}

static char *to_text(const Value *v) {
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  switch (v->kind) {
  case V_STR:
    return g_strdup(v->text);
  case V_NUM:
    g_ascii_formatd(buffer, sizeof(buffer), "%.15g", v->number);
    return g_strdup(buffer);
  case V_BOOL:
    return g_strdup(v->number != 0.0 ? "true" : "false");
  case V_NULL:
  default:
    return g_strdup("");
  }
}

static gboolean truthy(const Value *v) {
  double n;

  if (v->kind == V_BOOL || v->kind == V_NUM) {
    return v->number != 0.0;
  }
  if (v->kind == V_STR && to_number(v, &n)) {
    return n != 0.0;
  }
  return v->kind == V_STR && v->text[0] != '\0' && g_ascii_strcasecmp(v->text, "false") != 0;
}

static Value eval(const Node *node, const char *const *cells);

/* -1, 0, 1, or 2 when not comparable (NULL involved) */
static int compare_values(const Value *a, const Value *b) {
  double x, y;

  if (a->kind == V_NULL || b->kind == V_NULL) {
    return 2;
  }
  if (to_number(a, &x) && to_number(b, &y)) {
    return (x > y) - (x < y);
  }

  g_autofree char *left = to_text(a);
  g_autofree char *right = to_text(b);
  const int c = strcmp(left, right);

  return (c > 0) - (c < 0);
}

static gboolean like_match(const char *text, const char *pattern, gboolean fold) {
  while (*pattern != '\0') {
    if (*pattern == '%') {
      while (*pattern == '%') {
        pattern++;
      }
      if (*pattern == '\0') {
        return TRUE;
      }
      for (const char *t = text;; t = g_utf8_next_char(t)) {
        if (like_match(t, pattern, fold)) {
          return TRUE;
        }
        if (*t == '\0') {
          return FALSE;
        }
      }
    }
    if (*text == '\0') {
      return FALSE;
    }
    if (*pattern == '_') {
      text = g_utf8_next_char(text);
      pattern = g_utf8_next_char(pattern);
      continue;
    }
    {
      const gunichar a = g_utf8_get_char(text);
      const gunichar b = g_utf8_get_char(pattern);

      if (fold ? g_unichar_tolower(a) != g_unichar_tolower(b) : a != b) {
        return FALSE;
      }
    }
    text = g_utf8_next_char(text);
    pattern = g_utf8_next_char(pattern);
  }
  return *text == '\0';
}

static Value eval_function(const Node *node, const char *const *cells) {
  const guint n = node->children->len;
  Value args[3] = {{0}, {0}, {0}};
  Value result = null_value();
  double x;

  for (guint i = 0; i < n && i < 3; i++) {
    args[i] = eval(g_ptr_array_index(node->children, i), cells);
  }

  if (g_strcmp0(node->text, "coalesce") == 0) {
    for (guint i = 0; i < n; i++) {
      Value v = i < 3 ? args[i] : eval(g_ptr_array_index(node->children, i), cells);

      if (v.kind != V_NULL && !(v.kind == V_STR && v.text[0] == '\0')) {
        if (i < 3) {
          args[i].owned = FALSE;
        }
        result = v;
        break;
      }
      if (i >= 3) {
        value_clear(&v);
      }
    }
  } else if (n >= 1 && args[0].kind != V_NULL) {
    g_autofree char *text = to_text(&args[0]);

    if (g_strcmp0(node->text, "lower") == 0) {
      result = text_value(g_utf8_strdown(text, -1));
    } else if (g_strcmp0(node->text, "upper") == 0) {
      result = text_value(g_utf8_strup(text, -1));
    } else if (g_strcmp0(node->text, "trim") == 0) {
      result = text_value(g_strstrip(g_strdup(text)));
    } else if (g_strcmp0(node->text, "length") == 0) {
      result = number_value((double)g_utf8_strlen(text, -1));
    } else if (g_strcmp0(node->text, "abs") == 0 && to_number(&args[0], &x)) {
      result = number_value(fabs(x));
    } else if (g_strcmp0(node->text, "round") == 0 && to_number(&args[0], &x)) {
      double digits = 0;

      if (n >= 2) {
        to_number(&args[1], &digits);
      }
      result = number_value(round(x * pow(10, digits)) / pow(10, digits));
    } else if (g_strcmp0(node->text, "substr") == 0 && n >= 2) {
      double start = 1, count = G_MAXINT;
      const glong length = g_utf8_strlen(text, -1);

      to_number(&args[1], &start);
      if (n >= 3) {
        to_number(&args[2], &count);
      }
      start = CLAMP(start - 1, 0, length);
      count = CLAMP(count, 0, length - start);
      {
        const char *from = g_utf8_offset_to_pointer(text, (glong)start);
        const char *to = g_utf8_offset_to_pointer(from, (glong)count);

        result = text_value(g_strndup(from, (gsize)(to - from)));
      }
    }
  }

  for (guint i = 0; i < 3; i++) {
    value_clear(&args[i]);
  }
  return result;
}

static Value eval(const Node *node, const char *const *cells) {
  Value a = {0}, b = {0}, result = null_value();
  double x, y;

  switch (node->kind) {
  case N_NUMBER:
    return number_value(node->number);
  case N_STRING: {
    Value v = {.kind = V_STR, .text = node->text, .owned = FALSE};

    return v;
  }
  case N_BOOL:
    return bool_value(node->number != 0.0);
  case N_NULL:
    return null_value();
  case N_FIELD: {
    Value v = {.kind = V_STR, .text = (char *)cells[node->field], .owned = FALSE};

    if (v.text == NULL || v.text[0] == '\0') {
      return null_value();
    }
    return v;
  }
  case N_NEG:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    if (to_number(&a, &x)) {
      result = number_value(-x);
    }
    value_clear(&a);
    return result;
  case N_NOT:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    result = a.kind == V_NULL ? null_value() : bool_value(!truthy(&a));
    value_clear(&a);
    return result;
  case N_AND:
  case N_OR: {
    a = eval(g_ptr_array_index(node->children, 0), cells);
    const gboolean left = truthy(&a);
    const gboolean left_null = a.kind == V_NULL;

    value_clear(&a);
    if (node->kind == N_AND && !left && !left_null) {
      return bool_value(FALSE);
    }
    if (node->kind == N_OR && left) {
      return bool_value(TRUE);
    }
    b = eval(g_ptr_array_index(node->children, 1), cells);
    {
      const gboolean right = truthy(&b);
      const gboolean right_null = b.kind == V_NULL;

      value_clear(&b);
      if (node->kind == N_AND) {
        if (!right && !right_null) {
          return bool_value(FALSE);
        }
        return left_null || right_null ? null_value() : bool_value(TRUE);
      }
      if (right) {
        return bool_value(TRUE);
      }
      return left_null || right_null ? null_value() : bool_value(FALSE);
    }
  }
  case N_ADD:
  case N_SUB:
  case N_MUL:
  case N_DIV:
  case N_MOD:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    b = eval(g_ptr_array_index(node->children, 1), cells);
    if (to_number(&a, &x) && to_number(&b, &y)) {
      switch (node->kind) {
      case N_ADD:
        result = number_value(x + y);
        break;
      case N_SUB:
        result = number_value(x - y);
        break;
      case N_MUL:
        result = number_value(x * y);
        break;
      case N_DIV:
        result = y != 0.0 ? number_value(x / y) : null_value();
        break;
      case N_MOD:
        result = y != 0.0 ? number_value(fmod(x, y)) : null_value();
        break;
      default:
        break;
      }
    }
    value_clear(&a);
    value_clear(&b);
    return result;
  case N_CONCAT:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    b = eval(g_ptr_array_index(node->children, 1), cells);
    {
      g_autofree char *left = to_text(&a);
      g_autofree char *right = to_text(&b);

      result = text_value(g_strconcat(left, right, NULL));
    }
    value_clear(&a);
    value_clear(&b);
    return result;
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE: {
    int c;

    a = eval(g_ptr_array_index(node->children, 0), cells);
    b = eval(g_ptr_array_index(node->children, 1), cells);
    c = compare_values(&a, &b);
    value_clear(&a);
    value_clear(&b);
    if (c == 2) {
      return null_value();
    }
    switch (node->kind) {
    case N_EQ:
      return bool_value(c == 0);
    case N_NE:
      return bool_value(c != 0);
    case N_LT:
      return bool_value(c < 0);
    case N_LE:
      return bool_value(c <= 0);
    case N_GT:
      return bool_value(c > 0);
    case N_GE:
    default:
      return bool_value(c >= 0);
    }
  }
  case N_LIKE:
  case N_ILIKE:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    b = eval(g_ptr_array_index(node->children, 1), cells);
    if (a.kind != V_NULL && b.kind != V_NULL) {
      g_autofree char *text = to_text(&a);
      g_autofree char *pattern = to_text(&b);

      result = bool_value(like_match(text, pattern, node->kind == N_ILIKE));
    }
    value_clear(&a);
    value_clear(&b);
    return result;
  case N_IN:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    if (a.kind != V_NULL) {
      gboolean found = FALSE;

      for (guint i = 1; i < node->children->len && !found; i++) {
        b = eval(g_ptr_array_index(node->children, i), cells);
        found = compare_values(&a, &b) == 0;
        value_clear(&b);
      }
      result = bool_value(found);
    }
    value_clear(&a);
    return result;
  case N_BETWEEN: {
    Value c3;

    a = eval(g_ptr_array_index(node->children, 0), cells);
    b = eval(g_ptr_array_index(node->children, 1), cells);
    c3 = eval(g_ptr_array_index(node->children, 2), cells);
    {
      const int low = compare_values(&a, &b);
      const int high = compare_values(&a, &c3);

      if (low != 2 && high != 2) {
        result = bool_value(low >= 0 && high <= 0);
      }
    }
    value_clear(&a);
    value_clear(&b);
    value_clear(&c3);
    return result;
  }
  case N_IS_NULL:
  case N_IS_NOT_NULL:
    a = eval(g_ptr_array_index(node->children, 0), cells);
    result = bool_value((a.kind == V_NULL) == (node->kind == N_IS_NULL));
    value_clear(&a);
    return result;
  case N_FUNC:
    return eval_function(node, cells);
  default:
    return result;
  }
}

gboolean osmium_expression_matches(OsmiumExpression *expression, const char *const *cells) {
  Value v;
  gboolean matches;

  g_return_val_if_fail(expression != NULL, FALSE);

  v = eval(expression->root, cells);
  matches = v.kind != V_NULL && truthy(&v);
  value_clear(&v);
  return matches;
}
