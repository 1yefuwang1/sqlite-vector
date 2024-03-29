#include "virtual_table.h"

#include <exception>
#include <limits>
#include <string_view>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "hnswlib/hnswlib.h"
#include "index_options.h"
#include "macros.h"
#include "sqlite3ext.h"
#include "util.h"
#include "vector_space.h"

extern const sqlite3_api_routines* sqlite3_api;

namespace sqlite_vector {

enum ColumnIndexInTable {
  kColumnIndexVector,
  kColumnIndexDistance,
};

enum IndexConstraintUsage {
  kVector = 1,
  kRowid,
};

enum FunctionConstraint {
  kFunctionConstraintVectorSearchKnn = SQLITE_INDEX_CONSTRAINT_FUNCTION,
};

// Used to identify pointer type for sqlite_result_pointer/sqlite_value_pointer
static constexpr std::string_view kKnnParamType = "vector_search_knn_param";

struct KnnParam {
  Vector query_vector;
  uint32_t k;
};

// A helper function to reduce boilerplate code when setting zErrMsg.
static void SetZErrMsg(char** pzErr, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  if (*pzErr) {
    sqlite3_free(*pzErr);
  }
  *pzErr = sqlite3_vmprintf(fmt, args);

  va_end(args);
}

int VirtualTable::Create(sqlite3* db, void* pAux, int argc,
                         const char* const* argv, sqlite3_vtab** ppVTab,
                         char** pzErr) {
  int rc = sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);
  if (rc != SQLITE_OK) {
    return rc;
  }

  // The first string, argv[0], is the name of the module being invoked. The
  // module name is the name provided as the second argument to
  // sqlite3_create_module() and as the argument to the USING clause of the
  // CREATE VIRTUAL TABLE statement that is running. The second, argv[1], is the
  // name of the database in which the new virtual table is being created. The
  // database name is "main" for the primary database, or "temp" for TEMP
  // database, or the name given at the end of the ATTACH statement for attached
  // databases. The third element of the array, argv[2], is the name of the new
  // virtual table, as specified following the TABLE keyword in the CREATE
  // VIRTUAL TABLE statement. If present, the fourth and subsequent strings in
  // the argv[] array report the arguments to the module name in the CREATE
  // VIRTUAL TABLE statement.
  constexpr int kModuleParamOffset = 3;

  if (argc != 2 + kModuleParamOffset) {
    *pzErr = sqlite3_mprintf("Expected 3 argument, got %d",
                             argc - kModuleParamOffset);
    return SQLITE_ERROR;
  }

  std::string_view vector_space_str = argv[0 + kModuleParamOffset];
  DLOG(INFO) << "vector_space_str: " << vector_space_str;
  auto vector_space = NamedVectorSpace::FromString(vector_space_str);
  if (!vector_space.ok()) {
    *pzErr = sqlite3_mprintf("Invalid vector space: %s. Reason: %s",
                             argv[0 + kModuleParamOffset],
                             absl::StatusMessageAsCStr(vector_space.status()));
    return SQLITE_ERROR;
  }

  std::string_view index_options_str = argv[1 + kModuleParamOffset];
  DLOG(INFO) << "index_options_str: " << index_options_str;
  auto index_options = IndexOptions::FromString(index_options_str);
  if (!index_options.ok()) {
    *pzErr = sqlite3_mprintf("Invalid index_options %s. Reason: %s",
                             argv[1 + kModuleParamOffset],
                             absl::StatusMessageAsCStr(index_options.status()));
    return SQLITE_ERROR;
  }

  std::string sql = absl::StrFormat("CREATE TABLE X(%s, distance REAL hidden)",
                                    vector_space->vector_name);
  rc = sqlite3_declare_vtab(db, sql.c_str());
  DLOG(INFO) << "vtab declared: " << sql.c_str() << ", rc=" << rc;
  if (rc != SQLITE_OK) {
    return rc;
  }

  try {
    *ppVTab = new VirtualTable(std::move(*vector_space), *index_options);
  } catch (const std::exception& ex) {
    *pzErr = sqlite3_mprintf("Failed to create virtual table: %s", ex.what());
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

VirtualTable::~VirtualTable() {
  if (zErrMsg) {
    sqlite3_free(zErrMsg);
  }
}

int VirtualTable::Destroy(sqlite3_vtab* pVTab) {
  SQLITE_VECTOR_ASSERT(pVTab != nullptr);
  delete static_cast<VirtualTable*>(pVTab);
  return SQLITE_OK;
}

int VirtualTable::Open(sqlite3_vtab* pVtab, sqlite3_vtab_cursor** ppCursor) {
  SQLITE_VECTOR_ASSERT(pVtab != nullptr);
  SQLITE_VECTOR_ASSERT(ppCursor != nullptr);
  *ppCursor = new Cursor(static_cast<VirtualTable*>(pVtab));
  return SQLITE_OK;
}

int VirtualTable::Close(sqlite3_vtab_cursor* pCursor) {
  SQLITE_VECTOR_ASSERT(pCursor != nullptr);
  delete static_cast<Cursor*>(pCursor);
  return SQLITE_OK;
}

int VirtualTable::Rowid(sqlite3_vtab_cursor* pCur, sqlite_int64* pRowid) {
  SQLITE_VECTOR_ASSERT(pCur != nullptr);
  SQLITE_VECTOR_ASSERT(pRowid != nullptr);

  Cursor* cursor = static_cast<Cursor*>(pCur);
  if (cursor->current_row != cursor->result.cend()) {
    *pRowid = cursor->current_row->second;
    return SQLITE_OK;
  } else {
    return SQLITE_ERROR;
  }
}

int VirtualTable::Eof(sqlite3_vtab_cursor* pCur) {
  SQLITE_VECTOR_ASSERT(pCur != nullptr);

  Cursor* cursor = static_cast<Cursor*>(pCur);
  return cursor->current_row == cursor->result.cend();
}

int VirtualTable::Next(sqlite3_vtab_cursor* pCur) {
  SQLITE_VECTOR_ASSERT(pCur != nullptr);

  Cursor* cursor = static_cast<Cursor*>(pCur);
  if (cursor->current_row != cursor->result.cend()) {
    ++cursor->current_row;
  }

  return SQLITE_OK;
}

absl::StatusOr<Vector> VirtualTable::GetVectorByRowid(int64_t rowid) const {
  try {
    // TODO: handle cases where sizeof(rowid) != sizeof(hnswlib::labeltype)
    std::vector<float> vec =
        index_->getDataByLabel<float>(static_cast<hnswlib::labeltype>(rowid));
    SQLITE_VECTOR_ASSERT(vec.size() == dimension());
    return Vector(std::move(vec));
  } catch (const std::runtime_error& ex) {
    return absl::Status(absl::StatusCode::kNotFound, ex.what());
  }
}

int VirtualTable::Column(sqlite3_vtab_cursor* pCur, sqlite3_context* pCtx,
                         int N) {
  SQLITE_VECTOR_ASSERT(pCur != nullptr);
  SQLITE_VECTOR_ASSERT(pCtx != nullptr);

  Cursor* cursor = static_cast<Cursor*>(pCur);
  if (cursor->current_row == cursor->result.cend()) {
    return SQLITE_ERROR;
  }

  if (kColumnIndexDistance == N) {
    sqlite3_result_double(pCtx,
                          static_cast<double>(cursor->current_row->first));
    return SQLITE_OK;
  } else if (kColumnIndexVector == N) {
    Cursor::Rowid rowid = cursor->current_row->second;
    VirtualTable* vtab = static_cast<VirtualTable*>(pCur->pVtab);
    auto vector = vtab->GetVectorByRowid(rowid);
    if (vector.ok()) {
      std::string_view blob = vector->ToBlob();
      sqlite3_result_blob(pCtx, blob.data(), blob.size(), SQLITE_TRANSIENT);
      return SQLITE_OK;
    } else {
      std::string err =
          absl::StrFormat("Can't find vector with rowid %d", rowid);
      sqlite3_result_text(pCtx, err.c_str(), err.size(), SQLITE_TRANSIENT);
      return SQLITE_ERROR;
    }
  } else {
    std::string err = absl::StrFormat("Invalid column index: %d", N);
    sqlite3_result_text(pCtx, err.c_str(), err.size(), SQLITE_TRANSIENT);
    return SQLITE_ERROR;
  }
}

int VirtualTable::BestIndex(sqlite3_vtab* vtab,
                            sqlite3_index_info* index_info) {
  SQLITE_VECTOR_ASSERT(vtab != nullptr);
  SQLITE_VECTOR_ASSERT(index_info != nullptr);

  for (int i = 0; i < index_info->nConstraint; i++) {
    const auto& constraint = index_info->aConstraint[i];
    if (!constraint.usable) {
      continue;
    }
    int column = constraint.iColumn;
    if (constraint.op == kFunctionConstraintVectorSearchKnn &&
        column == kColumnIndexVector) {
      DLOG(INFO) << "Found vector search constraint";
      index_info->idxNum = IndexConstraintUsage::kVector;
      index_info->aConstraintUsage[i].argvIndex = 1;
      index_info->aConstraintUsage[i].omit = 1;
    } else if (column == -1) {
      // in this case the constraint is on rowid
      DLOG(INFO) << "Found rowid constraint";
      index_info->idxNum = IndexConstraintUsage::kRowid;
      index_info->aConstraintUsage[i].argvIndex = 2;
      index_info->aConstraintUsage[i].omit = 1;
    } else {
      DLOG(INFO) << "Unknown constraint iColumn=" << column
                 << ", op=" << constraint.op;
    }
  }

  return SQLITE_OK;
}

int VirtualTable::Filter(sqlite3_vtab_cursor* pCur, int idxNum,
                         const char* idxStr, int argc, sqlite3_value** argv) {
  SQLITE_VECTOR_ASSERT(pCur != nullptr);
  Cursor* cursor = static_cast<Cursor*>(pCur);
  SQLITE_VECTOR_ASSERT(pCur->pVtab != nullptr);

  DLOG(INFO) << "Filter called with idxNum=" << idxNum << ", idxStr=" << idxStr
             << ", argc=" << argc;

  VirtualTable* vtab = static_cast<VirtualTable*>(pCur->pVtab);

  if (idxNum == IndexConstraintUsage::kVector) {
    auto param = static_cast<KnnParam*>(
        sqlite3_value_pointer(argv[0], kKnnParamType.data()));
    if (param == nullptr) {
      SetZErrMsg(&vtab->zErrMsg,
                 "knn_param() should be used for the 2nd param of knn_search");

      return SQLITE_ERROR;
    }
    auto& query_vector = param->query_vector;
    uint32_t k = param->k;
    if (query_vector.dim() != vtab->dimension()) {
      SetZErrMsg(
          &vtab->zErrMsg,
          "Dimension mismatch: query vector has dimension %d, but the table "
          "has dimension %d",
          query_vector.dim(), vtab->dimension());
      return SQLITE_ERROR;
    } else {
      cursor->query_vector = std::move(
          vtab->space_.normalize ? query_vector.Normalize() : query_vector);
      auto knn = vtab->index_->searchKnnCloserFirst(
          cursor->query_vector.data().data(), k);

      SQLITE_VECTOR_ASSERT(cursor->result.empty());

      cursor->result = std::move(knn);
      cursor->current_row = cursor->result.cbegin();
      return SQLITE_OK;
    }

  } else {
    DLOG(INFO) << "Invalid idxNum: " << idxNum;
    SetZErrMsg(&vtab->zErrMsg, "Invalid index number: %d", idxNum);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

// a marker function with empty implementation
void KnnSearch(sqlite3_context* context, int argc, sqlite3_value** argv) {}

void KnnParamDeleter(void* param) {
  KnnParam* p = static_cast<KnnParam*>(param);
  delete p;
}

void KnnParamFunc(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 2) {
    sqlite3_result_error(ctx, "Number of parameter is not 2", -1);
    return;
  }

  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    sqlite3_result_error(ctx, "Vector(1st param) should be of type Blob", -1);
    return;
  }

  if (sqlite3_value_type(argv[1]) != SQLITE_INTEGER) {
    sqlite3_result_error(ctx, "k(2nd param) should be of type INTEGER", -1);
    return;
  }

  std::string_view vector_blob(
      reinterpret_cast<const char*>(sqlite3_value_blob(argv[0])),
      sqlite3_value_bytes(argv[0]));
  auto vec = Vector::FromBlob(vector_blob);
  if (!vec.ok()) {
    std::string err = absl::StrFormat("Failed to parse vector due to: %s",
                                      vec.status().message());
    sqlite3_result_error(ctx, err.c_str(), -1);
    return;
  }

  int32_t k = sqlite3_value_int(argv[1]);
  if (k <= 0) {
    sqlite3_result_error(ctx, "k should be greater than 0", -1);
    return;
  }

  KnnParam* param = new KnnParam();
  param->query_vector = std::move(*vec);
  param->k = static_cast<uint32_t>(k);

  sqlite3_result_pointer(ctx, param, kKnnParamType.data(), KnnParamDeleter);
  return;
}

int VirtualTable::FindFunction(sqlite3_vtab* pVtab, int nArg, const char* zName,
                               void (**pxFunc)(sqlite3_context*, int,
                                               sqlite3_value**),
                               void** ppArg) {
  SQLITE_VECTOR_ASSERT(pVtab != nullptr);
  if (std::string_view(zName) == "knn_search") {
    *pxFunc = KnnSearch;
    *ppArg = nullptr;
    return kFunctionConstraintVectorSearchKnn;
  }

  return 0;
}

// Only insert is supported for now
int VirtualTable::Update(sqlite3_vtab* pVTab, int argc, sqlite3_value** argv,
                         sqlite_int64* pRowid) {
  VirtualTable* vtab = static_cast<VirtualTable*>(pVTab);
  if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    // Insert with a new row
    if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
      SetZErrMsg(&vtab->zErrMsg, "rowid must be specified during insertion");
      return SQLITE_ERROR;
    }
    sqlite3_int64 raw_rowid = sqlite3_value_int64(argv[1]);
    // This limitation comes from the fact that rowid is used as the label in
    // hnswlib(hnswlib::labeltype), whose type is size_t. But rowid in sqlite3
    // has type int64.
    if (raw_rowid > std::numeric_limits<Cursor::Rowid>::max() ||
        raw_rowid < 0) {
      SetZErrMsg(&vtab->zErrMsg, "rowid %lld out of range", raw_rowid);
      return SQLITE_ERROR;
    }

    Cursor::Rowid rowid = static_cast<Cursor::Rowid>(raw_rowid);
    *pRowid = rowid;

    if (sqlite3_value_type(argv[2]) != SQLITE_BLOB) {
      SetZErrMsg(&vtab->zErrMsg, "vector must be of type Blob");
      return SQLITE_ERROR;
    }

    auto vector = Vector::FromBlob(std::string_view(
        reinterpret_cast<const char*>(sqlite3_value_blob(argv[2])),
        sqlite3_value_bytes(argv[2])));
    if (vector.ok()) {
      if (vector->dim() != vtab->dimension()) {
        SetZErrMsg(&vtab->zErrMsg,
                   "Dimension mismatch: vector has "
                   "dimension %d, but the table has "
                   "dimension %d",
                   vector->dim(), vtab->dimension());
        return SQLITE_ERROR;
      }

      vtab->index_->addPoint(vtab->space_.normalize
                                 ? vector->Normalize().data().data()
                                 : vector->data().data(),
                             static_cast<hnswlib::labeltype>(rowid));
      vtab->rowids_.insert(rowid);
      return SQLITE_OK;
    } else {
      SetZErrMsg(&vtab->zErrMsg, "Failed to perform insertion due to: %s",
                 absl::StatusMessageAsCStr(vector.status()));
      return SQLITE_ERROR;
    }
  } else {
    SetZErrMsg(&vtab->zErrMsg, "Operation not supported for now");
    return SQLITE_ERROR;
  }
}

}  // end namespace sqlite_vector
