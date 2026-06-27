// NOLINTBEGIN

#ifndef BuiltInRaw_H
#define BuiltInRaw_H

#include <vlink/base/bytes.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cdr/travoddscdr.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wsign-compare"

#if defined(DDS_TYPE_EXPORT) && defined(_WIN32)
#define DDS_TYPE_API __declspec(dllexport)
#else
#define DDS_TYPE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

namespace vlink {

class DDS_TYPE_API BuiltInRaw {
 public:
  bool operator<(const BuiltInRaw& rhs) const { return false; }
  unsigned long long id;
  Bytes data;
};

}  // namespace vlink

#ifdef __cplusplus
}
#endif
#endif /* BuiltInRaw_H */

// NOLINTEND