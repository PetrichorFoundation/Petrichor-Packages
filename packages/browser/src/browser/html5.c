/* HTML parsing with hubbub, NetSurf's HTML5 parser: the same tree-building rules as other modern browsers (implied
 * and misnested tags, tables, forms, character references, encodings).  hubbub calls us back to build the tree out
 * of our own nodes; dom_adopt() then does the rest. */
#include "web.h"
#define NS_NO_LIBC_DECLS
#include <hubbub/parser.h>
#include <hubbub/tree.h>

static struct page *HP;

static char *cstr_of(const hubbub_string *s)
{
    char *r = arena_alloc(HP, s->len + 1);
    if (r) {
        memcpy(r, s->ptr, s->len);
        r[s->len] = 0;
    }
    return r;
}

static void set_attrs(struct node *n, const hubbub_attribute *at, u32 count, int only_new)
{
    if (!count) return;
    struct attr *na = arena_alloc(HP, (n->nattr + count) * sizeof *na);
    if (!na) return;
    if (n->nattr) memcpy(na, n->attr, n->nattr * sizeof *na);
    int k = n->nattr;
    for (u32 i = 0; i < count; i++) {
        char *name = cstr_of(&at[i].name);
        if (!name) continue;
        for (char *c = name; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c += 32;
        if (only_new && attr_val(n, name)) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) if (!strcmp(na[j].name, name)) dup = 1;
        if (dup) continue;
        na[k].name = name;
        na[k].val = cstr_of(&at[i].value);
        if (!na[k].val) na[k].val = "";
        k++;
    }
    n->attr = na;
    n->nattr = k;
}

static void detach(struct node *n)
{
    struct node *par = n->parent;
    if (!par) return;
    if (n->prev) n->prev->next = n->next; else par->first = n->next;
    if (n->next) n->next->prev = n->prev; else par->last = n->prev;
    n->parent = n->prev = n->next = NULL;
}

static void link_before(struct node *parent, struct node *n, struct node *before)
{
    detach(n);
    n->parent = parent;
    if (before) {
        n->next = before;
        n->prev = before->prev;
        if (before->prev) before->prev->next = n; else parent->first = n;
        before->prev = n;
    } else {
        n->prev = parent->last;
        n->next = NULL;
        if (parent->last) parent->last->next = n; else parent->first = n;
        parent->last = n;
    }
}

/* Adjacent text is kept in one node, as hubbub expects. */
static struct node *merge_text(struct node *into, struct node *t)
{
    char *s = arena_alloc(HP, into->tlen + t->tlen + 1);
    if (!s) return into;
    memcpy(s, into->text, into->tlen);
    memcpy(s + into->tlen, t->text, t->tlen);
    s[into->tlen + t->tlen] = 0;
    into->text = s;
    into->tlen += t->tlen;
    return into;
}

static hubbub_error cb_comment(void *ctx, const hubbub_string *data, void **result)
{
    *result = dom_new_text(HP, "", 0);                     /* comments become empty text: nothing shows */
    return *result ? HUBBUB_OK : HUBBUB_NOMEM;
}

static hubbub_error cb_doctype(void *ctx, const hubbub_doctype *d, void **result)
{
    *result = dom_new_text(HP, "", 0);
    return *result ? HUBBUB_OK : HUBBUB_NOMEM;
}

static hubbub_error cb_element(void *ctx, const hubbub_tag *tag, void **result)
{
    char name[48];
    int l = tag->name.len < 47 ? (int)tag->name.len : 47;
    memcpy(name, tag->name.ptr, l);
    name[l] = 0;
    struct node *n = dom_new_element(HP, name);
    if (!n) return HUBBUB_NOMEM;
    set_attrs(n, tag->attributes, tag->n_attributes, 0);
    *result = n;
    return HUBBUB_OK;
}

static hubbub_error cb_text(void *ctx, const hubbub_string *data, void **result)
{
    *result = dom_new_text(HP, (const char *)data->ptr, data->len);
    return *result ? HUBBUB_OK : HUBBUB_NOMEM;
}

static hubbub_error cb_ref(void *ctx, void *node) { return HUBBUB_OK; }

static hubbub_error cb_append(void *ctx, void *parent, void *child, void **result)
{
    struct node *p = parent, *c = child;
    if (c->type == 1 && p->last && p->last->type == 1 && p->last != c && c->tlen) {
        *result = merge_text(p->last, c);
        return HUBBUB_OK;
    }
    link_before(p, c, NULL);
    *result = c;
    return HUBBUB_OK;
}

static hubbub_error cb_insert_before(void *ctx, void *parent, void *child, void *ref, void **result)
{
    struct node *p = parent, *c = child, *r = ref;
    if (c->type == 1 && r && r->prev && r->prev->type == 1 && c->tlen) {
        *result = merge_text(r->prev, c);
        return HUBBUB_OK;
    }
    link_before(p, c, r);
    *result = c;
    return HUBBUB_OK;
}

static hubbub_error cb_remove(void *ctx, void *parent, void *child, void **result)
{
    detach(child);
    *result = child;
    return HUBBUB_OK;
}

static hubbub_error cb_clone(void *ctx, void *node, bool deep, void **result)
{
    *result = dom_clone(HP, node, deep);
    return *result ? HUBBUB_OK : HUBBUB_NOMEM;
}

static hubbub_error cb_reparent(void *ctx, void *node, void *new_parent)
{
    struct node *n = node;
    while (n->first)
        link_before(new_parent, n->first, NULL);
    return HUBBUB_OK;
}

static hubbub_error cb_parent(void *ctx, void *node, bool element_only, void **result)
{
    struct node *p = ((struct node *)node)->parent;
    *result = element_only && p && p->type != 0 ? NULL : p;
    return HUBBUB_OK;
}

static hubbub_error cb_has_children(void *ctx, void *node, bool *result)
{
    *result = ((struct node *)node)->first != NULL;
    return HUBBUB_OK;
}

static hubbub_error cb_form(void *ctx, void *form, void *node) { return HUBBUB_OK; }

static hubbub_error cb_add_attrs(void *ctx, void *node, const hubbub_attribute *at, uint32_t n)
{
    set_attrs(node, at, n, 1);
    return HUBBUB_OK;
}

static hubbub_error cb_quirks(void *ctx, hubbub_quirks_mode mode) { return HUBBUB_OK; }
static hubbub_error cb_encoding(void *ctx, const char *name) { return HUBBUB_OK; }
static hubbub_error cb_script(void *ctx, void *script) { return HUBBUB_OK; }

int html5_parse(struct page *p, const char *html, int len)
{
    HP = p;
    struct node *doc = dom_new_element(p, "#document");
    if (!doc) return -1;
    hubbub_tree_handler th = {
        cb_comment, cb_doctype, cb_element, cb_text, cb_ref, cb_ref, cb_append, cb_insert_before, cb_remove,
        cb_clone, cb_reparent, cb_parent, cb_has_children, cb_form, cb_add_attrs, cb_quirks, cb_encoding,
        cb_script, NULL};
    hubbub_parser *parser;
    if (hubbub_parser_create("UTF-8", false, &parser) != HUBBUB_OK)
        return -1;
    hubbub_parser_optparams o;
    o.tree_handler = &th;
    int ok = hubbub_parser_setopt(parser, HUBBUB_PARSER_TREE_HANDLER, &o) == HUBBUB_OK;
    o.document_node = doc;
    ok = ok && hubbub_parser_setopt(parser, HUBBUB_PARSER_DOCUMENT_NODE, &o) == HUBBUB_OK;
    o.enable_scripting = true;                          /* <noscript> content is not shown: we run scripts */
    hubbub_parser_setopt(parser, HUBBUB_PARSER_ENABLE_SCRIPTING, &o);
    ok = ok && hubbub_parser_parse_chunk(parser, (const uint8_t *)html, len) == HUBBUB_OK;
    ok = ok && hubbub_parser_completed(parser) == HUBBUB_OK;
    hubbub_parser_destroy(parser);
    struct node *root = NULL;
    for (struct node *c = doc->first; c; c = c->next)
        if (c->type == 0 && c->tag == TG_HTML) root = c;
    if (!ok || !root)
        return -1;
    p->title[0] = 0;
    dom_adopt(p, root);
    return 0;
}
