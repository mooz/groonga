/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2010 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <string.h>
#include "db.h"
#include "module.h"
#include "ql.h"
#include "module.h"

static grn_hash *grn_modules = NULL;

#define PATHLEN(filename) (strlen(filename) + 1)

#ifdef WIN32
#  define grn_dl_open(filename)    LoadLibrary(filename)
#  define grn_dl_open_error_label  "LoadLibrary"
#  define grn_dl_close(dl)         (FreeLibrary(dl) != 0)
#  define grn_dl_close_error_label "FreeLibrary"
#  define grn_dl_sym(dl, symbol)   GetProcAddress(dl, symbol)
#  define grn_dl_sym_error_label   "GetProcAddress"
#  define grn_dl_clear_error
#else
#  include <dlfcn.h>
#  define grn_dl_open(filename)    dlopen(filename, RTLD_LAZY | RTLD_GLOBAL)
#  define grn_dl_open_error_label  dlerror()
#  define grn_dl_close(dl)         (dlclose(dl) == 0)
#  define grn_dl_close_error_label dlerror()
#  define grn_dl_sym(dl, symbol)   dlsym(dl, symbol)
#  define grn_dl_sym_error_label   dlerror()
#  define grn_dl_clear_error       dlerror()
#endif

grn_id
grn_module_get(grn_ctx *ctx, const char *filename)
{
  return grn_hash_get(ctx, grn_modules, filename, PATHLEN(filename), NULL);
}

const char *
grn_module_path(grn_ctx *ctx, grn_id id)
{
  uint32_t key_size;
  return _grn_hash_key(ctx, grn_modules, id, &key_size);
}

#define GRN_MODULE_FUNC_PREFIX "grn_module_"

static grn_rc
grn_module_initialize(grn_ctx *ctx, grn_module *module,
                      grn_dl dl, const char *path)
{
  const char *base_name, *extension;
  char init_func_name[PATH_MAX];
  char register_func_name[PATH_MAX];
  char fin_func_name[PATH_MAX];

  base_name = extension = path + strlen(path);
  for (; path < base_name; base_name--) {
    if (*base_name == '.') {
      extension = base_name;
    }
    if (*base_name == PATH_SEPARATOR[0]) {
      base_name++;
      break;
    }
  }

  module->dl = dl;

#define GET_SYMBOL(type)                                                \
  strcpy(type ## _func_name, GRN_MODULE_FUNC_PREFIX #type "_");         \
  strncat(type ## _func_name, base_name, extension - base_name);        \
  grn_dl_clear_error;                                                   \
  module->type ## _func = grn_dl_sym(dl, type ## _func_name);           \
  if (!module->type ## _func) {                                         \
    SERR(grn_dl_sym_error_label);                                       \
  }

  GET_SYMBOL(init);
  GET_SYMBOL(register);
  GET_SYMBOL(fin);

#undef GET_SYMBOL

  if (!module->init_func || !module->register_func || !module->fin_func) {
    ERR(GRN_INVALID_FORMAT,
        "init func (%s) %sfound, "
        "register func (%s) %sfound and "
        "fin func (%s) %sfound",
        init_func_name, module->init_func ? "" : "not ",
        register_func_name, module->register_func ? "" : "not ",
        fin_func_name, module->fin_func ? "" : "not ");
  }

  return ctx->rc;
}

grn_id
grn_module_open(grn_ctx *ctx, const char *filename)
{
  grn_id id;
  grn_dl dl;
  grn_module **module;

  if ((id = grn_hash_get(ctx, grn_modules, filename, PATHLEN(filename),
                         (void **)&module))) {
    return id;
  }
  if ((dl = grn_dl_open(filename))) {
    if ((id = grn_hash_add(ctx, grn_modules, filename, PATHLEN(filename),
                           (void **)&module, NULL))) {
      *module = GRN_GMALLOCN(grn_module, 1);
      if (*module) {
        if (grn_module_initialize(ctx, *module, dl, filename)) {
          GRN_GFREE(*module);
          *module = NULL;
        }
      }
      if (!*module) {
        grn_hash_delete_by_id(ctx, grn_modules, id, NULL);
        if (!grn_dl_close(dl)) {
          SERR(grn_dl_close_error_label);
        }
        id = GRN_ID_NIL;
      }
    } else {
      if (!grn_dl_close(dl)) {
        SERR(grn_dl_close_error_label);
      }
    }
  } else {
    SERR(grn_dl_open_error_label);
  }
  return id;
}

grn_rc
grn_module_close(grn_ctx *ctx, grn_id id)
{
  grn_module *module;

  grn_module_fin(ctx, id);
  if (!grn_hash_get_value(ctx, grn_modules, id, &module)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!grn_dl_close(module->dl)) {
    SERR(grn_dl_close_error_label);
  }
  GRN_GFREE(module);
  return grn_hash_delete_by_id(ctx, grn_modules, id, NULL);
}

void *
grn_module_sym(grn_ctx *ctx, grn_id id, const char *symbol)
{
  grn_module *module;
  grn_dl_symbol func;

  if (!grn_hash_get_value(ctx, grn_modules, id, &module)) {
    return NULL;
  }
  grn_dl_clear_error;
  if (!(func = grn_dl_sym(module->dl, symbol))) {
    SERR(grn_dl_sym_error_label);
  }
  return func;
}

grn_rc
grn_module_init (grn_ctx *ctx, grn_id id)
{
  grn_module *module;
  if (!grn_hash_get_value(ctx, grn_modules, id, &module)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (module->init_func) {
    return module->init_func(ctx);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_module_register (grn_ctx *ctx, grn_id id)
{
  grn_module *module;
  if (!grn_hash_get_value(ctx, grn_modules, id, &module)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (module->register_func) {
    return module->register_func(ctx);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_module_fin (grn_ctx *ctx, grn_id id)
{
  grn_module *module;
  if (!grn_hash_get_value(ctx, grn_modules, id, &module)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (module->fin_func) {
    return module->fin_func(ctx);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_modules_init(void)
{
  grn_modules = grn_hash_create(&grn_gctx, NULL, PATH_MAX, sizeof(grn_module *),
                                GRN_OBJ_KEY_VAR_SIZE);
  if (!grn_modules) { return GRN_NO_MEMORY_AVAILABLE; }
  return GRN_SUCCESS;
}

grn_rc
grn_modules_fin(void)
{
  grn_ctx *ctx = &grn_gctx;
  if (!grn_modules) { return GRN_INVALID_ARGUMENT; }
  GRN_HASH_EACH(ctx, grn_modules, id, NULL, NULL, NULL, {
      grn_module_close(ctx, id);
    });
  return grn_hash_close(&grn_gctx, grn_modules);
}

grn_obj *
grn_proc_create(grn_ctx *ctx, const char *name, unsigned name_size, grn_proc_type type,
                grn_proc_func *init, grn_proc_func *next, grn_proc_func *fin,
                unsigned nvars, grn_expr_var *vars)
{
  grn_proc *res = NULL;
  grn_id id = GRN_ID_NIL;
  grn_id range;
  int added = 0;
  grn_obj *db;
  const char *path = ctx->impl->module_path;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
  range = path ? grn_module_get(ctx, path) : GRN_ID_NIL;
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR();
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    GRN_API_RETURN(NULL);
  }
  if (name && name_size) {
    grn_db *s = (grn_db *)db;
    if (!(id = grn_pat_add(ctx, s->keys, name, name_size, NULL, &added))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "grn_pat_add failed");
      GRN_API_RETURN(NULL);
    }
    if (!added) {
      grn_proc **vp;
      if ((vp = grn_tiny_array_at(&s->values, id)) && (res = (*vp))) {
        if (res->funcs[PROC_INIT]) {
          ERR(GRN_INVALID_ARGUMENT, "already used name");
          GRN_API_RETURN(NULL);
        }
      } else {
        added = 1;
      }
    }
  } else if (ctx->impl && ctx->impl->values) {
    id = grn_array_add(ctx, ctx->impl->values, NULL) | GRN_OBJ_TMP_OBJECT;
    added = 1;
  }
  if (!res) { res = GRN_MALLOCN(grn_proc, 1); }
  if (res) {
    GRN_DB_OBJ_SET_TYPE(res, GRN_PROC);
    res->obj.db = db;
    res->obj.id = id;
    res->obj.header.domain = GRN_ID_NIL;
    res->obj.header.flags = path ? GRN_OBJ_CUSTOM_NAME : 0;
    res->obj.range = range;
    res->type = type;
    res->funcs[PROC_INIT] = init;
    res->funcs[PROC_NEXT] = next;
    res->funcs[PROC_FIN] = fin;
    GRN_TEXT_INIT(&res->name_buf, 0);
    res->vars = NULL;
    res->nvars = 0;
    if (added) {
      if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
        // grn_obj_delete(ctx, db, id);
        GRN_FREE(res);
        GRN_API_RETURN(NULL);
      }
    }
    while (nvars--) {
      grn_obj *v = grn_expr_add_var(ctx, (grn_obj *)res, vars->name, vars->name_size);
      GRN_OBJ_INIT(v, vars->value.header.type, 0, vars->value.header.domain);
      GRN_TEXT_PUT(ctx, v, GRN_TEXT_VALUE(&vars->value), GRN_TEXT_LEN(&vars->value));
      vars++;
    }
  }
  GRN_API_RETURN((grn_obj *)res);
}

grn_rc
grn_db_register(grn_ctx *ctx, const char *path)
{
  grn_id id;
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return ctx->rc;
  }
  GRN_API_ENTER;
  if (GRN_DB_P(db)) {
    char complemented_path[PATH_MAX];

    id = grn_module_open(ctx, path);
    if (!id) {
      ctx->errlvl = GRN_OK;
      ctx->rc = GRN_SUCCESS;
      strcpy(complemented_path, path);
      strncat(complemented_path, GRN_MODULE_SUFFIX, PATH_MAX - strlen(path) - 1);
      path = complemented_path;
      id = grn_module_open(ctx, path);
    }

    if (id) {
      ctx->impl->module_path = path;
      ctx->rc = grn_module_init(ctx, id);
      if (!ctx->rc) {
        ctx->rc = grn_module_register(ctx, id);
      }
      ctx->impl->module_path = NULL;
      if (ctx->rc) {
        grn_module_close(ctx, id);
      }
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
  }
  GRN_API_RETURN(ctx->rc);
}

static grn_rc
grn_db_register_by_name(grn_ctx *ctx, const char *name,
                        const char *env_name, const char *default_dir)
{
  const char *modules_dir;
  char dir_last_char;
  char path[PATH_MAX];

  modules_dir = getenv(env_name);
  if (!modules_dir) {
    modules_dir = default_dir;
  }
  strcpy(path, modules_dir);
  dir_last_char = modules_dir[strlen(modules_dir) - 1];
  if (dir_last_char != PATH_SEPARATOR[0]) {
    strcat(path, PATH_SEPARATOR);
  }
  strncat(path, name, PATH_MAX - strlen(modules_dir) - 1);
  return grn_db_register(ctx, path);
}

grn_rc
grn_db_register_tokenizer(grn_ctx *ctx, const char *name)
{
  return grn_db_register_by_name(ctx, name,
                                 "GRN_TOKENIZER_MODULES_DIR",
                                 TOKENIZER_MODULES_DIR);
}

grn_rc
grn_db_register_function(grn_ctx *ctx, const char *name)
{
  return grn_db_register_by_name(ctx, name,
                                 "GRN_FUNCTION_MODULES_DIR",
                                 FUNCTION_MODULES_DIR);
}