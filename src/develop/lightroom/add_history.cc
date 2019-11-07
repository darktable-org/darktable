#include "develop/lightroom/add_history.h"

#include <cstdint>
#include <sqlite3.h>

extern "C"
{
#include "common/darktable.h"
#include "common/debug.h"
#include "common/iop_order.h"
}

namespace lightroom
{

void add_history(int imgid, dt_develop_t const *dev, std::string const &operation_name, int version,
                 void const *params, int params_size)
{
  static constexpr int blend_version = 4;
  static constexpr int blendif_size = 16;
  struct blend_params_t
  {
    /** blending mode */
    uint32_t mode;
    /** mixing opacity */
    float opacity;
    /** id of mask in current pipeline */
    uint32_t mask_id;
    /** blendif mask */
    uint32_t blendif;
    /** blur radius */
    float radius;
    /** blendif parameters */
    float blendif_parameters[4 * blendif_size];
  };
  blend_params_t blend_params = { 0 };

  //  get current num if any
  int32_t num = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM main.history WHERE imgid = ?1",
                              -1, &stmt, nullptr);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // add new history info
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history (imgid, num, module, operation, op_params, enabled, "
                              "blendop_params, blendop_version, multi_priority, multi_name, iop_order) "
                              "VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, 0, ' ', ?8)",
                              -1, &stmt, nullptr);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation_name.c_str(), operation_name.length(), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, blend_version);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, dt_ioppr_get_iop_order(dev->iop_order_list, operation_name.c_str()));

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM "
                              "main.history WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, nullptr);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void remove_history(int imgid, std::string const &operation_name)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1 AND operation = ?2", -1, &stmt, nullptr);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation_name.c_str(), operation_name.length(), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also bump history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM "
                              "main.history WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, nullptr);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

}