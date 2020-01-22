/*
    This file is part of darktable,
    copyright (c) 2018 edgardo hoszowski.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_IOP_ORDER_H
#define DT_IOP_ORDER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;

typedef enum dt_iop_order_t
{
  DT_IOP_ORDER_CUSTOM  = 0, // a customr order (re-ordering the pipe)
  DT_IOP_ORDER_LEGACY  = 1, // up to dt 2.6.3
  DT_IOP_ORDER_V30     = 2, // starts with dt 3.0
  DT_IOP_ORDER_LAST    = 3
} dt_iop_order_t;

typedef struct dt_iop_order_entry_t
{
  union {
    double iop_order_f;  // only used for backward compatibility while migrating db
    int iop_order;       // order from 1 and incrementing
  } o;
  // operation + instance is the unique id for an active module in the pipe
  char operation[20];
  int32_t instance;      // or previously named multi_priority
} dt_iop_order_entry_t;

typedef struct dt_iop_order_rule_t
{
  char op_prev[20];
  char op_next[20];
} dt_iop_order_rule_t;

/** return the iop-order-version used by imgid (DT_IOP_ORDER_V30 if unknown iop-order-version) */
dt_iop_order_t dt_ioppr_get_iop_order_version(const int32_t imgid);
/** returns the kind of the list by looking at the order of the modules, it is either one of the built-in version
    or a customr order  */
dt_iop_order_t dt_ioppr_get_iop_order_list_kind(GList *iop_order_list);

/** returns a list of dt_iop_order_entry_t and updates *_version */
GList *dt_ioppr_get_iop_order_list(int32_t imgid, gboolean sorted);
/** return the iop-order list for the given version, this is used to get the built-in lists */
GList *dt_ioppr_get_iop_order_list_version(dt_iop_order_t version);
/** returns the dt_iop_order_entry_t of iop_order_list with operation = op_name */
dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name, const int multi_priority);
/** likewise, but returns the link in the list instead of the entry */
GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority);

/** returns the iop_order from iop_order_list list with operation = op_name */
int dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name, const int multi_priority);
/** returns TRUE if operation/multi-priority is before base_operation (first in pipe) on the iop-list */
gboolean dt_ioppr_is_iop_before(GList *iop_order_list, const char *base_operation,
                                const char *operation, const int multi_priority);
/* write iop-order list for the given image */
gboolean dt_ioppr_write_iop_order_list(GList *iop_order_list, const int32_t imgid);
gboolean dt_ioppr_write_iop_order(const dt_iop_order_t kind, GList *iop_order_list, const int32_t imgid);

/** serialize list, used for presets */
void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size);
/** returns the iop_order_list from the serialized form found in buf (blob in preset table) */
GList *dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size);
/** likewise but a text serializer/deserializer */
char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list);
GList *dt_ioppr_deserialize_text_iop_order_list(const char *buf);

/** insert a match for module into the iop-order list */
void dt_ioppr_insert_module_instance(struct dt_develop_t *dev, struct dt_iop_module_t *module);
void dt_ioppr_resync_modules_order(struct dt_develop_t *dev);
void dt_ioppr_resync_iop_list(struct dt_develop_t *dev);

/** update target_iop_order_list to ensure that modules in iop_order_list are in target_iop_order_list
    note that iop_order_list contains a set of dt_iop_order_entry_t where order is the multi-priority */
void dt_ioppr_update_for_entries(struct dt_develop_t *dev, GList *entry_list, gboolean append);
void dt_ioppr_update_for_style_items(struct dt_develop_t *dev, GList *st_items, gboolean append);
void dt_ioppr_update_for_modules(struct dt_develop_t *dev, GList *modules, gboolean append);

/** check if there's duplicate iop_order entries in iop_list */
void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list);

/** sets the default iop_order to iop_list */
void dt_ioppr_set_default_iop_order(struct dt_develop_t *dev, const int32_t imgid);
void dt_ioppr_migrate_iop_order(struct dt_develop_t *dev, const int32_t imgid);
/** extract all modules with multi-instances */
GList *dt_ioppr_extract_multi_instances_list(GList *iop_order_list);
/** merge all modules with multi-instances as extracted with routine above into a canonical iop-order list */
GList *dt_ioppr_merge_multi_instance_iop_order_list(GList *iop_order_list, GList *multi_instance_list);

/** returns 1 if there's a module_so without a iop_order defined */
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list);

/* returns a list of dt_iop_order_rule_t with the current iop order rules */
GList *dt_ioppr_get_iop_order_rules();

/** returns a duplicate of iop_order_list */
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list);

/** sort two modules by iop_order */
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b);
gint dt_sort_iop_list_by_order_f(gconstpointer a, gconstpointer b);

/** returns the iop_order before module_next if module can be moved */
gboolean dt_ioppr_check_can_move_before_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next);
/** returns the iop_order after module_prev if module can be moved */
gboolean dt_ioppr_check_can_move_after_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev);

/** moves module before/after module_next/previous on pipe */
gboolean dt_ioppr_move_iop_before(struct dt_develop_t *dev, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next);
gboolean dt_ioppr_move_iop_after(struct dt_develop_t *dev, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev);

// for debug only
int dt_ioppr_check_iop_order(struct dt_develop_t *dev, const int imgid, const char *msg);
void dt_ioppr_print_module_iop_order(GList *iop_list, const char *msg);
void dt_ioppr_print_history_iop_order(GList *history_list, const char *msg);
void dt_ioppr_print_iop_order(GList *iop_order_list, const char *msg);

#endif
