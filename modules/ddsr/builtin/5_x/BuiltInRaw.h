/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl using "rtiddsgen".
The rtiddsgen tool is part of the RTI Connext distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the RTI Connext manual.
*/

// NOLINTBEGIN

#ifndef BuiltInRaw_1859968709_h
#define BuiltInRaw_1859968709_h

#ifndef NDDS_STANDALONE_TYPE
#ifndef ndds_c_h
#include "ndds/ndds_c.h"
#endif
#else
#include "ndds_standalone_type.h"
#endif

extern const char* vlink_BuiltInRawTYPENAME;

typedef struct vlink_BuiltInRaw {
  DDS_UnsignedLongLong id;
  struct DDS_OctetSeq data;

} vlink_BuiltInRaw;
#if (defined(RTI_WIN32) || defined(RTI_WINCE)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, start exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport __declspec(dllexport)
#endif

NDDSUSERDllExport DDS_TypeCode* vlink_BuiltInRaw_get_typecode(void); /* Type code */

DDS_SEQUENCE(vlink_BuiltInRawSeq, vlink_BuiltInRaw);

NDDSUSERDllExport RTIBool vlink_BuiltInRaw_initialize(vlink_BuiltInRaw* self);

NDDSUSERDllExport RTIBool vlink_BuiltInRaw_initialize_ex(vlink_BuiltInRaw* self, RTIBool allocatePointers,
                                                            RTIBool allocateMemory);

NDDSUSERDllExport RTIBool vlink_BuiltInRaw_initialize_w_params(vlink_BuiltInRaw* self,
                                                                  const struct DDS_TypeAllocationParams_t* allocParams);

NDDSUSERDllExport void vlink_BuiltInRaw_finalize(vlink_BuiltInRaw* self);

NDDSUSERDllExport void vlink_BuiltInRaw_finalize_ex(vlink_BuiltInRaw* self, RTIBool deletePointers);

NDDSUSERDllExport void vlink_BuiltInRaw_finalize_w_params(vlink_BuiltInRaw* self,
                                                             const struct DDS_TypeDeallocationParams_t* deallocParams);

NDDSUSERDllExport void vlink_BuiltInRaw_finalize_optional_members(vlink_BuiltInRaw* self, RTIBool deletePointers);

NDDSUSERDllExport RTIBool vlink_BuiltInRaw_copy(vlink_BuiltInRaw* dst, const vlink_BuiltInRaw* src);

#if (defined(RTI_WIN32) || defined(RTI_WINCE)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, stop exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport
#endif

#endif /* BuiltInRaw */

// NOLINTEND
