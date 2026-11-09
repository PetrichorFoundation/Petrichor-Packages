/* NetSurf's CSS engine (libcss) working with the browser's own one.
 *
 * css.c computes every element's style first (it understands custom properties, grid, gradients, rounded corners
 * and `gap`).  Then libcss, which is the more complete and standards-exact engine for everything else (selectors,
 * the cascade, !important, inheritance, shorthands, units, @media), recomputes the page and its answers replace
 * ours property by property.  What libcss doesn't have stays as css.c computed it.  Pages whose stylesheets use
 * var() keep css.c's styles entirely, because libcss would drop those declarations. */
#include "web.h"
#define NS_NO_LIBC_DECLS
#include <libcss/libcss.h>
#include <strings.h>

static struct page *CP;
static css_unit_ctx unit_ctx;

/* ---------------- strings ---------------- */

static lwc_string *intern(const char *s)
{
    lwc_string *r = NULL;
    lwc_intern_string(s, strlen(s), &r);
    return r;
}

static int lwc_eq(lwc_string *a, const char *s)
{
    size_t n = lwc_string_length(a);
    return strlen(s) == n && !strncasecmp(lwc_string_data(a), s, n);
}

static int lwc_eq_exact(lwc_string *a, const char *s)
{
    size_t n = lwc_string_length(a);
    return strlen(s) == n && !memcmp(lwc_string_data(a), s, n);
}

/* ---------------- the select handler: libcss asks about our tree ---------------- */

static struct node *el_parent(struct node *n)
{
    struct node *p = n->parent;
    return p && p->type == 0 && n != CP->root ? p : NULL;
}

static struct node *el_prev(struct node *n)
{
    for (n = n->prev; n; n = n->prev)
        if (n->type == 0) return n;
    return NULL;
}

static int has_name(struct node *n, const css_qname *q)
{
    if (lwc_eq(q->name, "*")) return 1;
    return lwc_eq(q->name, dom_tag_name(n));
}

static css_error h_node_name(void *pw, void *node, css_qname *q)
{
    q->ns = NULL;
    q->name = intern(dom_tag_name(node));
    return q->name ? CSS_OK : CSS_NOMEM;
}

static css_error h_node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *n)
{
    *classes = NULL;
    *n = 0;
    const char *c = attr_val(node, "class");
    if (!c) return CSS_OK;
    int cap = 0;
    for (const char *p = c; *p; p++) if (*p != ' ' && (p == c || p[-1] == ' ')) cap++;
    if (!cap) return CSS_OK;
    lwc_string **arr = malloc(cap * sizeof *arr);
    if (!arr) return CSS_NOMEM;
    for (const char *p = c; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (e > p && (int)*n < cap) lwc_intern_string(p, e - p, &arr[(*n)++]);
        p = e;
    }
    *classes = arr;
    return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
    const char *v = attr_val(node, "id");
    *id = v && *v ? intern(v) : NULL;
    return CSS_OK;
}

static css_error h_named_ancestor(void *pw, void *node, const css_qname *q, void **out)
{
    *out = NULL;
    for (struct node *a = el_parent(node); a; a = el_parent(a))
        if (has_name(a, q)) { *out = a; break; }
    return CSS_OK;
}

static css_error h_named_parent(void *pw, void *node, const css_qname *q, void **out)
{
    struct node *p = el_parent(node);
    *out = p && has_name(p, q) ? p : NULL;
    return CSS_OK;
}

static css_error h_named_sibling(void *pw, void *node, const css_qname *q, void **out)
{
    struct node *s = el_prev(node);
    *out = s && has_name(s, q) ? s : NULL;
    return CSS_OK;
}

static css_error h_named_generic_sibling(void *pw, void *node, const css_qname *q, void **out)
{
    *out = NULL;
    for (struct node *s = el_prev(node); s; s = el_prev(s))
        if (has_name(s, q)) { *out = s; break; }
    return CSS_OK;
}

static css_error h_parent(void *pw, void *node, void **out) { *out = el_parent(node); return CSS_OK; }
static css_error h_sibling(void *pw, void *node, void **out) { *out = el_prev(node); return CSS_OK; }

static css_error h_has_name(void *pw, void *node, const css_qname *q, bool *m) { *m = has_name(node, q); return CSS_OK; }

static int word_in(const char *list, const char *w, int wl)
{
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (e - p == wl && !memcmp(p, w, wl)) return 1;
        p = e;
    }
    return 0;
}

static css_error h_has_class(void *pw, void *node, lwc_string *name, bool *m)
{
    *m = word_in(attr_val(node, "class"), lwc_string_data(name), lwc_string_length(name));
    return CSS_OK;
}

static css_error h_has_id(void *pw, void *node, lwc_string *name, bool *m)
{
    const char *v = attr_val(node, "id");
    *m = v && lwc_eq_exact(name, v);
    return CSS_OK;
}

static const char *attr_q(void *node, const css_qname *q)
{
    char name[64];
    size_t l = lwc_string_length(q->name);
    if (l >= sizeof name) return NULL;
    for (size_t i = 0; i < l; i++) {
        char c = lwc_string_data(q->name)[i];
        name[i] = c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    name[l] = 0;
    return attr_val(node, name);
}

static css_error h_has_attr(void *pw, void *node, const css_qname *q, bool *m) { *m = attr_q(node, q) != NULL; return CSS_OK; }

static css_error h_attr_equal(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    const char *a = attr_q(node, q);
    *m = a && lwc_eq_exact(v, a);
    return CSS_OK;
}

static css_error h_attr_dash(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    const char *a = attr_q(node, q);
    size_t l = lwc_string_length(v);
    *m = a && !strncmp(a, lwc_string_data(v), l) && (a[l] == 0 || a[l] == '-');
    return CSS_OK;
}

static css_error h_attr_includes(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    *m = word_in(attr_q(node, q), lwc_string_data(v), lwc_string_length(v));
    return CSS_OK;
}

static css_error h_attr_prefix(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    const char *a = attr_q(node, q);
    size_t l = lwc_string_length(v);
    *m = a && l && !strncmp(a, lwc_string_data(v), l);
    return CSS_OK;
}

static css_error h_attr_suffix(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    const char *a = attr_q(node, q);
    size_t l = lwc_string_length(v), al = a ? strlen(a) : 0;
    *m = a && l && al >= l && !memcmp(a + al - l, lwc_string_data(v), l);
    return CSS_OK;
}

static css_error h_attr_substring(void *pw, void *node, const css_qname *q, lwc_string *v, bool *m)
{
    const char *a = attr_q(node, q);
    size_t l = lwc_string_length(v);
    *m = 0;
    if (a && l)
        for (const char *p = a; *p && !*m; p++)
            if (!strncmp(p, lwc_string_data(v), l)) *m = 1;
    return CSS_OK;
}

static css_error h_is_root(void *pw, void *node, bool *m) { *m = node == CP->root; return CSS_OK; }

static css_error h_count_siblings(void *pw, void *node, bool same_name, bool after, int32_t *count)
{
    struct node *n = node;
    int c = 0;
    const char *name = dom_tag_name(n);
    for (struct node *s = after ? n->next : n->prev; s; s = after ? s->next : s->prev)
        if (s->type == 0 && (!same_name || !strcmp(dom_tag_name(s), name))) c++;
    *count = c;
    return CSS_OK;
}

static css_error h_is_empty(void *pw, void *node, bool *m)
{
    *m = 1;
    for (struct node *c = ((struct node *)node)->first; c; c = c->next)
        if (c->type == 0 || c->tlen) { *m = 0; break; }
    return CSS_OK;
}

static css_error h_is_link(void *pw, void *node, bool *m)
{
    struct node *n = node;
    *m = n->tag == TG_A && attr_val(n, "href");
    return CSS_OK;
}

static css_error h_false(void *pw, void *node, bool *m) { *m = 0; return CSS_OK; }

static css_error h_is_hover(void *pw, void *node, bool *m)
{
    struct node *x = css_hover;
    while (x && x != node) x = x->parent;
    *m = x != NULL;
    return CSS_OK;
}

static css_error h_is_focus(void *pw, void *node, bool *m) { *m = node == css_focus; return CSS_OK; }

static int form_control(struct node *n)
{
    return n->tag == TG_INPUT || n->tag == TG_BUTTON || n->tag == TG_SELECT || n->tag == TG_TEXTAREA;
}

static css_error h_is_enabled(void *pw, void *node, bool *m) { *m = form_control(node) && !attr_val(node, "disabled"); return CSS_OK; }
static css_error h_is_disabled(void *pw, void *node, bool *m) { *m = form_control(node) && attr_val(node, "disabled"); return CSS_OK; }
static css_error h_is_checked(void *pw, void *node, bool *m) { *m = attr_val(node, "checked") != NULL; return CSS_OK; }
static css_error h_is_lang(void *pw, void *node, lwc_string *lang, bool *m) { *m = 0; return CSS_OK; }

static css_error h_hints(void *pw, void *node, uint32_t *n, css_hint **hints)
{
    *n = 0;                                          /* presentational attributes: css.c's values are merged in */
    *hints = NULL;
    return CSS_OK;
}

static css_error h_ua_default(void *pw, uint32_t prop, css_hint *hint)
{
    if (prop == CSS_PROP_COLOR) {
        hint->data.color = 0xFF000000;
        hint->status = CSS_COLOR_COLOR;
    } else if (prop == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
    } else if (prop == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
    } else if (prop == CSS_PROP_VOICE_FAMILY) {
        hint->data.strings = NULL;
        hint->status = 0;
    } else {
        return CSS_INVALID;
    }
    return CSS_OK;
}

static css_select_handler handler;

static css_error h_set_data(void *pw, void *node, void *data)
{
    if (data)                                        /* no style sharing cache: drop it right away */
        css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, pw, node, NULL, data);
    return CSS_OK;
}

static css_error h_get_data(void *pw, void *node, void **data) { *data = NULL; return CSS_OK; }

static css_select_handler handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    h_node_name, h_node_classes, h_node_id,
    h_named_ancestor, h_named_parent, h_named_sibling, h_named_generic_sibling,
    h_parent, h_sibling,
    h_has_name, h_has_class, h_has_id, h_has_attr, h_attr_equal, h_attr_dash, h_attr_includes, h_attr_prefix,
    h_attr_suffix, h_attr_substring,
    h_is_root, h_count_siblings, h_is_empty,
    h_is_link, h_false /* visited */, h_is_hover, h_false /* active */, h_is_focus,
    h_is_enabled, h_is_disabled, h_is_checked,
    h_false /* target */, h_is_lang,
    h_hints, h_ua_default,
    h_set_data, h_get_data,
};

/* ---------------- stylesheets ---------------- */

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
    char r[512], out[512];
    size_t l = lwc_string_length(rel);
    if (l >= sizeof r) l = sizeof r - 1;
    memcpy(r, lwc_string_data(rel), l);
    r[l] = 0;
    url_resolve(base && *base ? base : CP->url, r, out, sizeof out);
    *abs = intern(out);
    return *abs ? CSS_OK : CSS_NOMEM;
}

static css_stylesheet *make_sheet(const char *text, int len, const char *url, int is_inline)
{
    css_stylesheet_params params;
    memset(&params, 0, sizeof params);
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_DEFAULT;
    params.charset = "UTF-8";
    params.url = url ? url : CP->url;
    params.inline_style = is_inline;
    params.resolve = resolve_url;
    css_stylesheet *sheet;
    if (css_stylesheet_create(&params, &sheet) != CSS_OK)
        return NULL;
    css_error e = css_stylesheet_append_data(sheet, (const uint8_t *)text, len);
    if (e != CSS_OK && e != CSS_NEEDDATA) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }
    e = css_stylesheet_data_done(sheet);
    if (e != CSS_OK && e != CSS_IMPORTS_PENDING) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }
    return sheet;
}

/* ---------------- converting computed values ---------------- */

static int px_of(const css_computed_style *cs, css_fixed v, css_unit u)
{
    css_fixed px = css_unit_len2device_px(cs, &unit_ctx, v, u);
    return FIXTOINT(px);
}

static struct len len_of(const css_computed_style *cs, uint8_t kind, uint8_t set, uint8_t is_auto, css_fixed v, css_unit u)
{
    if (kind == set) {
        if (u == CSS_UNIT_PCT) return (struct len){(int)((v * 10) >> CSS_RADIX_POINT), L_PCT};
        return (struct len){px_of(cs, v, u), L_PX};
    }
    (void)is_auto;
    return (struct len){0, L_AUTO};
}

#define LEN(acc, SET, AUTO) ({ css_fixed _v = 0; css_unit _u = CSS_UNIT_PX; uint8_t _k = acc(cs, &_v, &_u); len_of(cs, _k, SET, AUTO, _v, _u); })

static int border_w(const css_computed_style *cs, uint8_t k, css_fixed v, css_unit u)
{
    if (k == CSS_BORDER_WIDTH_THIN) return 1;
    if (k == CSS_BORDER_WIDTH_MEDIUM) return 3;
    if (k == CSS_BORDER_WIDTH_THICK) return 5;
    return px_of(cs, v, u);
}

static int bstyle(uint8_t k)
{
    switch (k) {
    case CSS_BORDER_STYLE_NONE: case CSS_BORDER_STYLE_HIDDEN: return BS_NONE;
    case CSS_BORDER_STYLE_DOTTED: return BS_DOTTED;
    case CSS_BORDER_STYLE_DASHED: return BS_DASHED;
    default: return BS_SOLID;
    }
}

/* Fills s from libcss's style; `ours` is what css.c computed for the same element (kept where libcss is silent). */
static void convert(struct node *n, const css_computed_style *cs, struct style *s, const struct style *ours)
{
    switch (css_computed_display(cs, n == CP->root)) {
    case CSS_DISPLAY_NONE: s->display = D_NONE; break;
    case CSS_DISPLAY_BLOCK: case CSS_DISPLAY_RUN_IN: s->display = D_BLOCK; break;
    case CSS_DISPLAY_INLINE_BLOCK: case CSS_DISPLAY_INLINE_TABLE: s->display = D_INLINE_BLOCK; break;
    case CSS_DISPLAY_LIST_ITEM: s->display = D_LIST_ITEM; break;
    case CSS_DISPLAY_TABLE: s->display = D_TABLE; break;
    case CSS_DISPLAY_TABLE_ROW_GROUP: case CSS_DISPLAY_TABLE_HEADER_GROUP: case CSS_DISPLAY_TABLE_FOOTER_GROUP:
        s->display = D_ROW_GROUP; break;
    case CSS_DISPLAY_TABLE_ROW: s->display = D_ROW; break;
    case CSS_DISPLAY_TABLE_CELL: s->display = D_CELL; break;
    case CSS_DISPLAY_TABLE_CAPTION: s->display = D_CAPTION; break;
    case CSS_DISPLAY_TABLE_COLUMN: case CSS_DISPLAY_TABLE_COLUMN_GROUP: s->display = D_COL; break;
    case CSS_DISPLAY_FLEX: s->display = D_FLEX; break;
    case CSS_DISPLAY_INLINE_FLEX: s->display = D_INLINE_FLEX; break;
    case CSS_DISPLAY_GRID: case CSS_DISPLAY_INLINE_GRID: s->display = D_GRID; break;
    default: s->display = D_INLINE; break;
    }
    switch (css_computed_position(cs)) {
    case CSS_POSITION_RELATIVE: case CSS_POSITION_STICKY: s->position = POS_RELATIVE; break;
    case CSS_POSITION_ABSOLUTE: s->position = POS_ABSOLUTE; break;
    case CSS_POSITION_FIXED: s->position = POS_FIXED; break;
    default: s->position = POS_STATIC;
    }
    uint8_t fl = css_computed_float(cs);
    s->float_ = fl == CSS_FLOAT_LEFT ? FL_LEFT : fl == CSS_FLOAT_RIGHT ? FL_RIGHT : FL_NONE;
    uint8_t cl = css_computed_clear(cs);
    s->clear = cl == CSS_CLEAR_LEFT ? 1 : cl == CSS_CLEAR_RIGHT ? 2 : cl == CSS_CLEAR_BOTH ? 3 : 0;
    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER: case CSS_TEXT_ALIGN_LIBCSS_CENTER: s->text_align = TA_CENTER; break;
    case CSS_TEXT_ALIGN_RIGHT: case CSS_TEXT_ALIGN_LIBCSS_RIGHT: s->text_align = TA_RIGHT; break;
    case CSS_TEXT_ALIGN_JUSTIFY: s->text_align = TA_JUSTIFY; break;
    default: s->text_align = TA_LEFT;
    }
    switch (css_computed_white_space(cs)) {
    case CSS_WHITE_SPACE_PRE: s->white_space = WS_PRE; break;
    case CSS_WHITE_SPACE_NOWRAP: s->white_space = WS_NOWRAP; break;
    case CSS_WHITE_SPACE_PRE_WRAP: s->white_space = WS_PRE_WRAP; break;
    case CSS_WHITE_SPACE_PRE_LINE: s->white_space = WS_PRE_LINE; break;
    default: s->white_space = WS_NORMAL;
    }
    switch (css_computed_text_transform(cs)) {
    case CSS_TEXT_TRANSFORM_UPPERCASE: s->text_transform = TT_UPPER; break;
    case CSS_TEXT_TRANSFORM_LOWERCASE: s->text_transform = TT_LOWER; break;
    case CSS_TEXT_TRANSFORM_CAPITALIZE: s->text_transform = TT_CAP; break;
    default: s->text_transform = TT_NONE;
    }
    switch (css_computed_list_style_type(cs)) {
    case CSS_LIST_STYLE_TYPE_CIRCLE: s->list_style = LS_CIRCLE; break;
    case CSS_LIST_STYLE_TYPE_SQUARE: s->list_style = LS_SQUARE; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL: case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO: s->list_style = LS_DECIMAL; break;
    case CSS_LIST_STYLE_TYPE_NONE: s->list_style = LS_NONE; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ALPHA: case CSS_LIST_STYLE_TYPE_UPPER_ALPHA: case CSS_LIST_STYLE_TYPE_LOWER_LATIN:
    case CSS_LIST_STYLE_TYPE_UPPER_LATIN: s->list_style = LS_ALPHA; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ROMAN: case CSS_LIST_STYLE_TYPE_UPPER_ROMAN: s->list_style = LS_ROMAN; break;
    default: s->list_style = LS_DISC;
    }
    s->visibility = css_computed_visibility(cs) != CSS_VISIBILITY_VISIBLE;
    s->box_sizing = css_computed_box_sizing(cs) == CSS_BOX_SIZING_BORDER_BOX;
    switch (css_computed_flex_direction(cs)) {
    case CSS_FLEX_DIRECTION_ROW_REVERSE: s->flex_dir = FD_ROW_REV; break;
    case CSS_FLEX_DIRECTION_COLUMN: s->flex_dir = FD_COL; break;
    case CSS_FLEX_DIRECTION_COLUMN_REVERSE: s->flex_dir = FD_COL_REV; break;
    default: s->flex_dir = FD_ROW;
    }
    s->flex_wrap = css_computed_flex_wrap(cs) != CSS_FLEX_WRAP_NOWRAP;
    switch (css_computed_justify_content(cs)) {
    case CSS_JUSTIFY_CONTENT_FLEX_END: s->justify = JC_END; break;
    case CSS_JUSTIFY_CONTENT_CENTER: s->justify = JC_CENTER; break;
    case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: s->justify = JC_BETWEEN; break;
    case CSS_JUSTIFY_CONTENT_SPACE_AROUND: s->justify = JC_AROUND; break;
    case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: s->justify = JC_EVENLY; break;
    default: s->justify = JC_START;
    }
    static const u8 ai_map[] = {AI_STRETCH, AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE, AI_AUTO};
    uint8_t ai = css_computed_align_items(cs), as = css_computed_align_self(cs);
    s->align_items = ai < 7 ? ai_map[ai] : AI_STRETCH;
    s->align_self = as < 7 ? ai_map[as] : AI_AUTO;
    css_fixed v; css_unit u;
    switch (css_computed_vertical_align(cs, &v, &u)) {
    case CSS_VERTICAL_ALIGN_TOP: case CSS_VERTICAL_ALIGN_TEXT_TOP: s->vertical_align = VA_TOP; break;
    case CSS_VERTICAL_ALIGN_MIDDLE: s->vertical_align = VA_MIDDLE; break;
    case CSS_VERTICAL_ALIGN_BOTTOM: case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: s->vertical_align = VA_BOTTOM; break;
    default: s->vertical_align = VA_BASELINE;
    }
    uint8_t td = css_computed_text_decoration(cs);
    s->decor = (td & CSS_TEXT_DECORATION_UNDERLINE ? DEC_UNDERLINE : 0) | (td & CSS_TEXT_DECORATION_LINE_THROUGH ? DEC_STRIKE : 0);
    uint8_t fw = css_computed_font_weight(cs);
    s->bold = fw == CSS_FONT_WEIGHT_BOLD || fw == CSS_FONT_WEIGHT_BOLDER || fw >= CSS_FONT_WEIGHT_600;
    s->italic = css_computed_font_style(cs) != CSS_FONT_STYLE_NORMAL;
    lwc_string **fam = NULL;
    uint8_t gen = css_computed_font_family(cs, &fam);
    int mono = gen == CSS_FONT_FAMILY_MONOSPACE;
    for (int i = 0; fam && fam[i] && !mono; i++) {
        const char *d = lwc_string_data(fam[i]);
        size_t l = lwc_string_length(fam[i]);
        for (size_t k = 0; k + 4 <= l; k++)
            if (!strncasecmp(d + k, "mono", 4) || (k + 7 <= l && !strncasecmp(d + k, "courier", 7))) mono = 1;
    }
    s->mono = mono;
    int serif = gen == CSS_FONT_FAMILY_SERIF;
    if (fam && fam[0] && !mono) {                    /* the first named family decides, as css.c does */
        const char *d = lwc_string_data(fam[0]);
        size_t l = lwc_string_length(fam[0]);
        char low[64];
        size_t k = l < 63 ? l : 63;
        for (size_t i = 0; i < k; i++) low[i] = d[i] >= 'A' && d[i] <= 'Z' ? d[i] + 32 : d[i];
        low[k] = 0;
        serif = (strstr(low, "serif") && !strstr(low, "sans")) || strstr(low, "times") || strstr(low, "georgia") ||
                strstr(low, "garamond") || strstr(low, "palatino") || strstr(low, "cambria") || strstr(low, "book");
    }
    s->serif = !mono && serif;
    s->overflow = css_computed_overflow_x(cs) == CSS_OVERFLOW_VISIBLE ? OV_VISIBLE : OV_HIDDEN;
    s->collapse = css_computed_border_collapse(cs) == CSS_BORDER_COLLAPSE_COLLAPSE;

    css_color c;
    css_computed_color(cs, &c);
    s->color = c;
    css_computed_background_color(cs, &c);
    s->bg = c;

    css_computed_font_size(cs, &v, &u);
    s->font_size = px_of(cs, v, u);
    if (s->font_size < 1) s->font_size = 1;
    switch (css_computed_line_height(cs, &v, &u)) {
    case CSS_LINE_HEIGHT_NUMBER: s->line_height = (int)(((long long)v * s->font_size) >> CSS_RADIX_POINT); break;
    case CSS_LINE_HEIGHT_DIMENSION:
        s->line_height = u == CSS_UNIT_PCT ? (int)(((long long)v * s->font_size / 100) >> CSS_RADIX_POINT) : px_of(cs, v, u);
        break;
    default: s->line_height = 0;
    }

    s->width = LEN(css_computed_width, CSS_WIDTH_SET, CSS_WIDTH_AUTO);
    s->height = LEN(css_computed_height, CSS_HEIGHT_SET, CSS_HEIGHT_AUTO);
    s->min_w = LEN(css_computed_min_width, CSS_MIN_WIDTH_SET, CSS_MIN_WIDTH_AUTO);
    s->min_h = LEN(css_computed_min_height, CSS_MIN_HEIGHT_SET, CSS_MIN_HEIGHT_AUTO);
    s->max_w = LEN(css_computed_max_width, CSS_MAX_WIDTH_SET, CSS_MAX_WIDTH_NONE);
    s->max_h = LEN(css_computed_max_height, CSS_MAX_HEIGHT_SET, CSS_MAX_HEIGHT_NONE);
    if (s->min_w.unit == L_AUTO) s->min_w = (struct len){0, L_PX};
    if (s->min_h.unit == L_AUTO) s->min_h = (struct len){0, L_PX};
    if (s->min_w.unit == L_PX && s->min_w.v == 0) s->min_w = ours->min_w;   /* keep css.c's "auto" convention */
    if (s->min_h.unit == L_PX && s->min_h.v == 0) s->min_h = ours->min_h;
    s->margin[0] = LEN(css_computed_margin_top, CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    s->margin[1] = LEN(css_computed_margin_right, CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    s->margin[2] = LEN(css_computed_margin_bottom, CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    s->margin[3] = LEN(css_computed_margin_left, CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    s->padding[0] = LEN(css_computed_padding_top, CSS_PADDING_SET, 99);
    s->padding[1] = LEN(css_computed_padding_right, CSS_PADDING_SET, 99);
    s->padding[2] = LEN(css_computed_padding_bottom, CSS_PADDING_SET, 99);
    s->padding[3] = LEN(css_computed_padding_left, CSS_PADDING_SET, 99);
    for (int i = 0; i < 4; i++) if (s->padding[i].unit == L_AUTO) s->padding[i] = (struct len){0, L_PX};

    uint8_t (*bws[4])(const css_computed_style *, css_fixed *, css_unit *) = {
        css_computed_border_top_width, css_computed_border_right_width, css_computed_border_bottom_width,
        css_computed_border_left_width};
    uint8_t (*bss[4])(const css_computed_style *) = {
        css_computed_border_top_style, css_computed_border_right_style, css_computed_border_bottom_style,
        css_computed_border_left_style};
    uint8_t (*bcs[4])(const css_computed_style *, css_color *) = {
        css_computed_border_top_color, css_computed_border_right_color, css_computed_border_bottom_color,
        css_computed_border_left_color};
    for (int i = 0; i < 4; i++) {
        s->border_style[i] = bstyle(bss[i](cs));
        uint8_t k = bws[i](cs, &v, &u);
        s->border_w[i] = s->border_style[i] == BS_NONE ? 0 : border_w(cs, k, v, u);
        bcs[i](cs, &c);
        s->border_color[i] = c;
    }

    switch (css_computed_flex_basis(cs, &v, &u)) {
    case CSS_FLEX_BASIS_SET: s->flex_basis = u == CSS_UNIT_PCT ? (struct len){(int)((v * 10) >> CSS_RADIX_POINT), L_PCT}
                                                               : (struct len){px_of(cs, v, u), L_PX}; break;
    default: s->flex_basis = (struct len){0, L_AUTO};
    }
    css_fixed f;
    css_computed_flex_grow(cs, &f);
    s->grow = (int)(((long long)f * 1000) >> CSS_RADIX_POINT);
    css_computed_flex_shrink(cs, &f);
    s->shrink = (int)(((long long)f * 1000) >> CSS_RADIX_POINT);
    int32_t ord = 0;
    css_computed_order(cs, &ord);
    s->order = ord;
    css_computed_opacity(cs, &f);
    s->opacity = (int)(((long long)f * 255) >> CSS_RADIX_POINT);
    if (s->opacity > 255) s->opacity = 255;
    if (s->opacity < 0) s->opacity = 0;

    /* What libcss doesn't model: css.c's values stand.  Presentational attributes (bgcolor, align, width on cells,
     * cellpadding, <font>) also came from css.c, used where libcss left the default. */
    s->radius = ours->radius;
    s->gap_r = ours->gap_r;
    s->gap_c = ours->gap_c;
    s->grid_cols = ours->grid_cols;
    memcpy(s->grid_col, ours->grid_col, sizeof s->grid_col);
    s->vars = ours->vars;
    s->nvars = ours->nvars;
    s->bg_kind = ours->bg_kind;
    s->bg2 = ours->bg2;
    s->bg_img = ours->bg_img;
    s->bg_size = ours->bg_size;
    s->bg_px = ours->bg_px;
    s->bg_py = ours->bg_py;
    if (ours->bg_kind != BG_NONE || (!(s->bg >> 24) && (ours->bg >> 24))) s->bg = ours->bg;
    if (s->width.unit == L_AUTO) s->width = ours->width;
    if (s->height.unit == L_AUTO) s->height = ours->height;
    if (attr_val(n, "align") && s->text_align == TA_LEFT) s->text_align = ours->text_align;
    if (s->float_ == FL_NONE) s->float_ = ours->float_;
    if ((n->tag == TG_TD || n->tag == TG_TH || n->tag == TG_TABLE) && !s->border_w[0] && ours->border_w[0])
        for (int i = 0; i < 4; i++) {
            s->border_w[i] = ours->border_w[i];
            s->border_style[i] = ours->border_style[i];
            s->border_color[i] = ours->border_color[i];
        }
    if ((n->tag == TG_TD || n->tag == TG_TH) && s->padding[0].unit == L_PX && !s->padding[0].v)
        memcpy(s->padding, ours->padding, sizeof s->padding);
    if (n->tag == TG_FONT || attr_val(n, "nowrap") || (n->tag == TG_BODY && attr_val(n, "text"))) {
        if (n->tag == TG_FONT) { s->color = ours->color; s->font_size = ours->font_size; }
        if (attr_val(n, "nowrap")) s->white_space = WS_NOWRAP;
        if (n->tag == TG_BODY && attr_val(n, "text")) s->color = ours->color;
    }

    /* the same structural rules as css.c */
    if (n->tag == TG_NOSCRIPT || s->position == POS_ABSOLUTE || s->position == POS_FIXED)
        s->display = D_NONE;
    if (s->float_ != FL_NONE && s->display == D_INLINE)
        s->display = D_BLOCK;
}

/* ---------------- walking the page ---------------- */

static int restyle_node(css_select_ctx *ctx, const css_media *media, struct node *n, const css_computed_style *parent)
{
    css_stylesheet *inl = NULL;
    const char *st = attr_val(n, "style");
    if (st && *st)
        inl = make_sheet(st, strlen(st), NULL, 1);
    css_select_results *res = NULL;
    css_error e = css_select_style(ctx, n, &unit_ctx, media, inl, &handler, NULL, &res);
    if (e != CSS_OK || !res || !res->styles[CSS_PSEUDO_ELEMENT_NONE]) {
        if (inl) css_stylesheet_destroy(inl);
        if (res) css_select_results_destroy(res);
        return -1;
    }
    css_computed_style *mine = res->styles[CSS_PSEUDO_ELEMENT_NONE], *composed = NULL;
    if (parent) {
        if (css_computed_style_compose(parent, mine, &unit_ctx, &composed) != CSS_OK) composed = NULL;
    }
    const css_computed_style *use = composed ? composed : mine;
    if (!parent) unit_ctx.root_style = use;
    if (n->st) {
        struct style ours = *n->st;
        convert(n, use, n->st, &ours);
    }
    int err = 0;
    for (struct node *c = n->first; c && !err; c = c->next)
        if (c->type == 0)
            err = restyle_node(ctx, media, c, use);
    if (composed) css_computed_style_destroy(composed);
    css_select_results_destroy(res);
    if (inl) css_stylesheet_destroy(inl);
    return err;
}

static int uses_vars(const char *t, int len)
{
    for (int i = 0; i + 4 <= len; i++)
        if (t[i] == 'v' && !memcmp(t + i, "var(", 4)) return 1;
    return 0;
}

int cssns_used;                                     /* the last restyle went through libcss */

/* Called by css_restyle() after css.c styled the page.  Returns 0 if libcss restyled it. */
int cssns_restyle(struct page *p, int viewport_w, const char *ua, int ua_len)
{
    cssns_used = 0;
    if (!p->root) return -1;
    CP = p;
    /* the page's stylesheets, in order; any var() and css.c keeps the page */
    const char *texts[70];
    int lens[70];
    const char *urls[70];
    int ns = 0;
    for (int i = 0; i < p->nsheets && ns < 70; i++) {
        const char *t = p->sheets[i].text;
        int len = p->sheets[i].len;
        const char *url = NULL;
        if (p->sheets[i].res) {
            struct res *r = &p->res[p->sheets[i].res - 1];
            if (r->state != RS_DONE || !r->css) continue;
            t = r->css;
            len = r->css_len;
            url = r->url;
        }
        if (!t) continue;
        if (uses_vars(t, len)) return -1;
        texts[ns] = t;
        lens[ns] = len;
        urls[ns] = url;
        ns++;
    }
    css_select_ctx *ctx;
    if (css_select_ctx_create(&ctx) != CSS_OK) return -1;
    css_stylesheet *sheets[71];
    int nsh = 0;
    css_stylesheet *ua_sheet = make_sheet(ua, ua_len, "about:ua", 0);
    if (ua_sheet && css_select_ctx_append_sheet(ctx, ua_sheet, CSS_ORIGIN_UA, NULL) == CSS_OK)
        sheets[nsh++] = ua_sheet;
    for (int i = 0; i < ns; i++) {
        css_stylesheet *sh = make_sheet(texts[i], lens[i], urls[i], 0);
        if (sh && css_select_ctx_append_sheet(ctx, sh, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK)
            sheets[nsh++] = sh;
        else if (sh)
            css_stylesheet_destroy(sh);
    }
    memset(&unit_ctx, 0, sizeof unit_ctx);
    unit_ctx.viewport_width = INTTOFIX(viewport_w);
    unit_ctx.viewport_height = INTTOFIX(600);
    unit_ctx.font_size_default = INTTOFIX(16);
    unit_ctx.device_dpi = INTTOFIX(96);
    css_media media;
    memset(&media, 0, sizeof media);
    media.type = CSS_MEDIA_SCREEN;
    media.width = INTTOFIX(viewport_w);
    media.height = INTTOFIX(600);
    int err = restyle_node(ctx, &media, p->root, NULL);
    css_select_ctx_destroy(ctx);
    for (int i = 0; i < nsh; i++)
        css_stylesheet_destroy(sheets[i]);
    cssns_used = err == 0;
    return err;
}
