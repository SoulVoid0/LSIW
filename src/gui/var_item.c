/* LSIW Control: LsiwVarItem, one entry of the LSIW variable table (a
 * Windows folder or value such as %APPDATA% or CSIDL_PERSONAL) wrapped as a
 * GObject for the Locations page's list model.
 *
 * The table entry itself is static; the item caches what depends on the
 * configuration (Windows path, Linux location, default/custom) and exposes
 * it as read-only properties, so cells bound with GtkExpressions update
 * when lsiw_var_item_refresh() notices a change.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <string.h>

struct _LsiwVarItem {
    GObject parent_instance;
    const struct lsiw_var *var;
    char *aliases;           /* other names: "%APPDATA%", "→ CSIDL_X"    */
    char *title_markup;      /* name in bold, then the aliases           */
    char *windows_path;      /* NULL when the variable has no value      */
    char *linux_path;        /* NULL for plain values and shell objects  */
    char *override;          /* the configured value, NULL = default     */
    char *tooltip;
    char *search_key;
};

G_DEFINE_FINAL_TYPE(LsiwVarItem, lsiw_var_item, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_NAME,
    PROP_TITLE_MARKUP,
    PROP_DESCRIPTION,
    PROP_KIND,
    PROP_SCOPE,
    PROP_WINDOWS_PATH,
    PROP_LINUX_PATH,
    PROP_STATUS,
    PROP_STATUS_MARKUP,
    PROP_CUSTOM,
    PROP_TOOLTIP,
    N_PROPS
};
static GParamSpec *props[N_PROPS];

const char *lsiw_var_kind_label(int kind)
{
    switch (kind) {
    case LSIW_VAR_FOLDER:  return "Folder";
    case LSIW_VAR_VIRTUAL: return "Shell object";
    case LSIW_VAR_VALUE:   return "Value";
    case LSIW_VAR_ALIAS:   return "Alias";
    case LSIW_VAR_DERIVED: return "Derived";
    default:               return "?";
    }
}

char *lsiw_var_parent_name(const struct lsiw_var *v)
{
    v = lsiw_var_resolve(v);
    if (!v || !v->def || strncmp(v->def, "${VAR:", 6)) return NULL;
    const char *e = strchr(v->def + 6, '}');
    return e ? g_strndup(v->def + 6, (gsize)(e - v->def - 6)) : NULL;
}

static int effective_kind(const struct lsiw_var *v)
{
    const struct lsiw_var *r = lsiw_var_resolve(v);
    return r ? r->kind : v->kind;
}

/* Names programs can also use for this variable: ALIAS rows that point to
 * it. Environment-style names are shown as %NAME%. */
static char *build_aliases(const struct lsiw_var *v)
{
    GString *al = g_string_new(NULL);
    if (v->kind == LSIW_VAR_ALIAS) {
        g_string_append_printf(al, "→ %s", v->alias_of);
    } else {
        size_t n;
        const struct lsiw_var *t = lsiw_vars(&n);
        for (size_t i = 0; i < n; i++) {
            if (t[i].kind != LSIW_VAR_ALIAS || g_ascii_strcasecmp(t[i].alias_of, v->name)) continue;
            if (al->len) g_string_append_c(al, ' ');
            if (g_str_has_prefix(t[i].name, "CSIDL_")) g_string_append(al, t[i].name);
            else g_string_append_printf(al, "%%%s%%", t[i].name);
        }
    }
    return g_string_free(al, FALSE);
}

static char *build_title_markup(const struct lsiw_var *v, const char *aliases)
{
    char *name = g_markup_escape_text(v->name, -1);
    char *res;
    if (*aliases) {
        char *esc = g_markup_escape_text(aliases, -1);
        res = g_strdup_printf("<b>%s</b>  <span alpha=\"60%%\">%s</span>", name, esc);
        g_free(esc);
    } else {
        res = g_strdup_printf("<b>%s</b>", name);
    }
    g_free(name);
    return res;
}

static void lsiw_var_item_finalize(GObject *obj)
{
    LsiwVarItem *self = LSIW_VAR_ITEM(obj);
    g_free(self->aliases);
    g_free(self->title_markup);
    g_free(self->windows_path);
    g_free(self->linux_path);
    g_free(self->override);
    g_free(self->tooltip);
    g_free(self->search_key);
    G_OBJECT_CLASS(lsiw_var_item_parent_class)->finalize(obj);
}

static const char *or_dash(const char *s)
{
    return s ? s : "—";
}

static void lsiw_var_item_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
    LsiwVarItem *self = LSIW_VAR_ITEM(obj);
    const struct lsiw_var *v = self->var;
    switch (id) {
    case PROP_NAME:          g_value_set_string(value, v->name); break;
    case PROP_TITLE_MARKUP:  g_value_set_string(value, self->title_markup); break;
    case PROP_DESCRIPTION:   g_value_set_string(value, v->desc); break;
    case PROP_KIND:          g_value_set_string(value, lsiw_var_kind_label(v->kind)); break;
    case PROP_SCOPE:
        g_value_set_string(value, v->scope == LSIW_SCOPE_USER ? "user" : "system");
        break;
    case PROP_WINDOWS_PATH:  g_value_set_string(value, or_dash(self->windows_path)); break;
    case PROP_LINUX_PATH:    g_value_set_string(value, or_dash(self->linux_path)); break;
    case PROP_STATUS:
        g_value_set_string(value, self->override ? "custom" : "default");
        break;
    case PROP_STATUS_MARKUP:
        g_value_set_string(value, self->override ? "<b>custom</b>"
                                                 : "<span alpha=\"60%\">default</span>");
        break;
    case PROP_CUSTOM:        g_value_set_boolean(value, self->override != NULL); break;
    case PROP_TOOLTIP:       g_value_set_string(value, self->tooltip); break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void lsiw_var_item_class_init(LsiwVarItemClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = lsiw_var_item_finalize;
    oc->get_property = lsiw_var_item_get_property;

    const GParamFlags f = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
    props[PROP_NAME] = g_param_spec_string("name", NULL, NULL, NULL, f);
    props[PROP_TITLE_MARKUP] = g_param_spec_string("title-markup", NULL, NULL, NULL, f);
    props[PROP_DESCRIPTION] = g_param_spec_string("description", NULL, NULL, NULL, f);
    props[PROP_KIND] = g_param_spec_string("kind", NULL, NULL, NULL, f);
    props[PROP_SCOPE] = g_param_spec_string("scope", NULL, NULL, NULL, f);
    props[PROP_WINDOWS_PATH] = g_param_spec_string("windows-path", NULL, NULL, NULL, f);
    props[PROP_LINUX_PATH] = g_param_spec_string("linux-path", NULL, NULL, NULL, f);
    props[PROP_STATUS] = g_param_spec_string("status", NULL, NULL, NULL, f);
    props[PROP_STATUS_MARKUP] = g_param_spec_string("status-markup", NULL, NULL, NULL, f);
    props[PROP_CUSTOM] = g_param_spec_boolean("custom", NULL, NULL, FALSE, f);
    props[PROP_TOOLTIP] = g_param_spec_string("tooltip", NULL, NULL, NULL, f);
    g_object_class_install_properties(oc, N_PROPS, props);
}

static void lsiw_var_item_init(LsiwVarItem *self)
{
    (void)self;
}

/* Replace a cached string; TRUE if it changed. */
static gboolean set_str(char **slot, const char *value)
{
    if (!g_strcmp0(*slot, value)) return FALSE;
    g_free(*slot);
    *slot = g_strdup(value);
    return TRUE;
}

static void rebuild_texts(LsiwVarItem *self)
{
    const struct lsiw_var *v = self->var;
    const struct lsiw_var *r = lsiw_var_resolve(v);
    int kind = effective_kind(v);
    GString *t = g_string_new(NULL);

    g_string_append_printf(t, "%s: %s\n", v->name, v->desc);
    if (v->kind == LSIW_VAR_ALIAS && r)
        g_string_append_printf(t, "Another name for %s (%s, %s scope)\n", r->name,
                               lsiw_var_kind_label(r->kind),
                               r->scope == LSIW_SCOPE_USER ? "user" : "system");
    else
        g_string_append_printf(t, "%s, %s scope\n", lsiw_var_kind_label(v->kind),
                               v->scope == LSIW_SCOPE_USER ? "user" : "system");
    if (self->windows_path)
        g_string_append_printf(t, "%s: %s\n", kind == LSIW_VAR_VALUE ? "Value" : "Windows path",
                               self->windows_path);
    if (self->linux_path) g_string_append_printf(t, "Linux location: %s\n", self->linux_path);
    if (r && r->csidl >= 0) g_string_append_printf(t, "CSIDL: 0x%04X (%d)\n", r->csidl, r->csidl);
    if (r && r->shell_value)
        g_string_append_printf(t, "Registry \"Shell Folders\" value: %s\n", r->shell_value);
    if (r && r->anchor) g_string_append_printf(t, "Always appears as C:\\%s\n", r->anchor);
    if (r && r->def && *r->def) g_string_append_printf(t, "Default: %s\n", r->def);
    if (self->override) g_string_append_printf(t, "Configured: %s\n", self->override);
    g_string_append(t, v->usmt ? "Listed in Microsoft's USMT documentation"
                               : "A standard Windows variable that USMT does not list");
    set_str(&self->tooltip, t->str);
    g_string_free(t, TRUE);

    char *raw = g_strjoin("\n", v->name, self->aliases, v->desc,
                          lsiw_var_kind_label(v->kind),
                          self->windows_path ? self->windows_path : "",
                          self->linux_path ? self->linux_path : "", NULL);
    g_free(self->search_key);
    self->search_key = g_utf8_casefold(raw, -1);
    g_free(raw);
}

gboolean lsiw_var_item_refresh(LsiwVarItem *self)
{
    const struct lsiw_var *v = self->var;
    int kind = effective_kind(v);
    /* The lsiw_var_* results point into caches that live until the next
     * configuration change: copy them right away. */
    const char *win = lsiw_var_windows(v->name);
    gboolean cw = set_str(&self->windows_path, win);
    const char *lin = kind == LSIW_VAR_VALUE ? NULL : lsiw_var_linux(v->name);
    gboolean cl = set_str(&self->linux_path, lin);
    /* config.c ignores an override for a DERIVED variable. */
    const char *ov = kind == LSIW_VAR_DERIVED ? NULL : lsiw_var_override(v->name);
    gboolean co = set_str(&self->override, ov);
    if (!(cw || cl || co) && self->tooltip) return FALSE;

    rebuild_texts(self);
    GObject *o = G_OBJECT(self);
    g_object_freeze_notify(o);
    if (cw) g_object_notify_by_pspec(o, props[PROP_WINDOWS_PATH]);
    if (cl) g_object_notify_by_pspec(o, props[PROP_LINUX_PATH]);
    if (co) {
        g_object_notify_by_pspec(o, props[PROP_STATUS]);
        g_object_notify_by_pspec(o, props[PROP_STATUS_MARKUP]);
        g_object_notify_by_pspec(o, props[PROP_CUSTOM]);
    }
    g_object_notify_by_pspec(o, props[PROP_TOOLTIP]);
    g_object_thaw_notify(o);
    return TRUE;
}

LsiwVarItem *lsiw_var_item_new(const struct lsiw_var *var)
{
    LsiwVarItem *self = g_object_new(LSIW_TYPE_VAR_ITEM, NULL);
    self->var = var;
    self->aliases = build_aliases(var);
    self->title_markup = build_title_markup(var, self->aliases);
    lsiw_var_item_refresh(self);
    return self;
}

const struct lsiw_var *lsiw_var_item_get_var(LsiwVarItem *item) { return item->var; }
int lsiw_var_item_effective_kind(LsiwVarItem *item) { return effective_kind(item->var); }
const char *lsiw_var_item_get_windows_path(LsiwVarItem *item) { return item->windows_path; }
const char *lsiw_var_item_get_linux_path(LsiwVarItem *item) { return item->linux_path; }
gboolean lsiw_var_item_is_custom(LsiwVarItem *item) { return item->override != NULL; }
const char *lsiw_var_item_get_search_key(LsiwVarItem *item) { return item->search_key; }
