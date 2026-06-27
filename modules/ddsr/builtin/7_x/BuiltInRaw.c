/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl
using RTI Code Generator (rtiddsgen) version 4.3.0.
The rtiddsgen tool is part of the RTI Connext DDS distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the Code Generator User's Manual.
*/

// NOLINTBEGIN

#ifndef NDDS_STANDALONE_TYPE
#ifndef ndds_c_h
#include "ndds/ndds_c.h"
#endif

#ifndef dds_c_log_infrastructure_h
#include "dds_c/dds_c_infrastructure_impl.h"
#endif

#ifndef cdr_type_h
#include "cdr/cdr_type.h"
#endif

#ifndef osapi_heap_h
#include "osapi/osapi_heap.h"
#endif
#else
#include "ndds_standalone_type.h"
#endif

#include "BuiltInRaw.h"

#ifndef NDDS_STANDALONE_TYPE
#include "BuiltInRawPlugin.h"
#endif

/* ========================================================================= */
const char *vlink_BuiltInRawTYPENAME = "vlink::dds_::BuiltInRaw_";

#ifndef NDDS_STANDALONE_TYPE
DDS_TypeCode *vlink_BuiltInRaw_get_typecode(void) {
  static RTIBool is_initialized = RTI_FALSE;

  static DDS_TypeCode vlink_BuiltInRaw_g_tc_data_sequence = DDS_INITIALIZE_SEQUENCE_TYPECODE(RTIXCdrLong_MAX, NULL);

  static DDS_TypeCode_Member vlink_BuiltInRaw_g_tc_members[2] = {

      {(char *)"id", /* Member name */
       {
           0,                 /* Representation ID */
           DDS_BOOLEAN_FALSE, /* Is a pointer? */
           -1,                /* Bitfield bits */
           NULL               /* Member type code is assigned later */
       },
       0,                       /* Ignored */
       0,                       /* Ignored */
       0,                       /* Ignored */
       NULL,                    /* Ignored */
       RTI_CDR_REQUIRED_MEMBER, /* Is a key? */
       DDS_PUBLIC_MEMBER,       /* Member visibility */
       1,
       NULL, /* Ignored */
       RTICdrTypeCodeAnnotations_INITIALIZER},
      {(char *)"data", /* Member name */
       {
           1,                 /* Representation ID */
           DDS_BOOLEAN_FALSE, /* Is a pointer? */
           -1,                /* Bitfield bits */
           NULL               /* Member type code is assigned later */
       },
       0,                       /* Ignored */
       0,                       /* Ignored */
       0,                       /* Ignored */
       NULL,                    /* Ignored */
       RTI_CDR_REQUIRED_MEMBER, /* Is a key? */
       DDS_PUBLIC_MEMBER,       /* Member visibility */
       1,
       NULL, /* Ignored */
       RTICdrTypeCodeAnnotations_INITIALIZER}};

  static DDS_TypeCode vlink_BuiltInRaw_g_tc = {{
      DDS_TK_STRUCT,                                           /* Kind */
      DDS_BOOLEAN_FALSE,                                       /* Ignored */
      -1,                                                      /*Ignored*/
      (char *)"vlink::dds_::BuiltInRaw_",                         /* Name */
      NULL,                                                    /* Ignored */
      0,                                                       /* Ignored */
      0,                                                       /* Ignored */
      NULL,                                                    /* Ignored */
      2,                                                       /* Number of members */
      vlink_BuiltInRaw_g_tc_members,                        /* Members */
      DDS_VM_NONE,                                             /* Ignored */
      RTICdrTypeCodeAnnotations_INITIALIZER, DDS_BOOLEAN_TRUE, /* _isCopyable */
      NULL,                                                    /* _sampleAccessInfo: assigned later */
      NULL                                                     /* _typePlugin: assigned later */
  }};                                                          /* Type code for vlink_BuiltInRaw*/

  if (is_initialized) {
    return &vlink_BuiltInRaw_g_tc;
  }

  is_initialized = RTI_TRUE;

  vlink_BuiltInRaw_g_tc._data._annotations._allowedDataRepresentationMask = 5;

  vlink_BuiltInRaw_g_tc_data_sequence._data._typeCode = (RTICdrTypeCode *)&DDS_g_tc_octet;
  vlink_BuiltInRaw_g_tc_data_sequence._data._sampleAccessInfo = &DDS_g_sai_seq;
  vlink_BuiltInRaw_g_tc_members[0]._representation._typeCode = (RTICdrTypeCode *)&DDS_g_tc_ulonglong;
  vlink_BuiltInRaw_g_tc_members[1]._representation._typeCode =
      (RTICdrTypeCode *)&vlink_BuiltInRaw_g_tc_data_sequence;

  /* Initialize the values for member annotations. */
  vlink_BuiltInRaw_g_tc_members[0]._annotations._defaultValue._d = RTI_XCDR_TK_ULONGLONG;
  vlink_BuiltInRaw_g_tc_members[0]._annotations._defaultValue._u.ulong_long_value = 0ull;
  vlink_BuiltInRaw_g_tc_members[0]._annotations._minValue._d = RTI_XCDR_TK_ULONGLONG;
  vlink_BuiltInRaw_g_tc_members[0]._annotations._minValue._u.ulong_long_value = RTIXCdrUnsignedLongLong_MIN;
  vlink_BuiltInRaw_g_tc_members[0]._annotations._maxValue._d = RTI_XCDR_TK_ULONGLONG;
  vlink_BuiltInRaw_g_tc_members[0]._annotations._maxValue._u.ulong_long_value = RTIXCdrUnsignedLongLong_MAX;

  vlink_BuiltInRaw_g_tc._data._sampleAccessInfo = vlink_BuiltInRaw_get_sample_access_info();
  vlink_BuiltInRaw_g_tc._data._typePlugin = vlink_BuiltInRaw_get_type_plugin_info();

  return &vlink_BuiltInRaw_g_tc;
}

RTIXCdrSampleAccessInfo *vlink_BuiltInRaw_get_sample_access_info(void) {
  static RTIBool is_initialized = RTI_FALSE;

  static RTIXCdrMemberAccessInfo vlink_BuiltInRaw_g_memberAccessInfos[2] = {RTIXCdrMemberAccessInfo_INITIALIZER};

  static RTIXCdrSampleAccessInfo vlink_BuiltInRaw_g_sampleAccessInfo = RTIXCdrSampleAccessInfo_INITIALIZER;

  if (is_initialized) {
    return (RTIXCdrSampleAccessInfo *)&vlink_BuiltInRaw_g_sampleAccessInfo;
  }

  vlink_BuiltInRaw_g_memberAccessInfos[0].bindingMemberValueOffset[0] = offsetof(struct vlink_BuiltInRaw, id);

  vlink_BuiltInRaw_g_memberAccessInfos[1].bindingMemberValueOffset[0] = offsetof(struct vlink_BuiltInRaw, data);

  vlink_BuiltInRaw_g_sampleAccessInfo.memberAccessInfos = vlink_BuiltInRaw_g_memberAccessInfos;

  {
    size_t candidateTypeSize = sizeof(vlink_BuiltInRaw);

    if (candidateTypeSize > RTIXCdrLong_MAX) {
      vlink_BuiltInRaw_g_sampleAccessInfo.typeSize[0] = RTIXCdrLong_MAX;
    } else {
      vlink_BuiltInRaw_g_sampleAccessInfo.typeSize[0] = (RTIXCdrUnsignedLong)candidateTypeSize;
    }
  }

  vlink_BuiltInRaw_g_sampleAccessInfo.languageBinding = RTI_XCDR_TYPE_BINDING_C;

  is_initialized = RTI_TRUE;
  return (RTIXCdrSampleAccessInfo *)&vlink_BuiltInRaw_g_sampleAccessInfo;
}
RTIXCdrTypePlugin *vlink_BuiltInRaw_get_type_plugin_info(void) {
  static RTIXCdrTypePlugin vlink_BuiltInRaw_g_typePlugin = {
      NULL, /* serialize */
      NULL, /* serialize_key */
      NULL, /* deserialize_sample */
      NULL, /* deserialize_key_sample */
      NULL, /* skip */
      NULL, /* get_serialized_sample_size */
      NULL, /* get_serialized_sample_max_size_ex */
      NULL, /* get_serialized_key_max_size_ex */
      NULL, /* get_serialized_sample_min_size */
      NULL, /* serialized_sample_to_key */
      (RTIXCdrTypePluginInitializeSampleFunction)vlink_BuiltInRaw_initialize_ex,
      NULL,
      (RTIXCdrTypePluginFinalizeSampleFunction)vlink_BuiltInRaw_finalize_w_return,
      NULL,
      NULL};

  return &vlink_BuiltInRaw_g_typePlugin;
}
#endif

RTIBool vlink_BuiltInRaw_initialize(vlink_BuiltInRaw *sample) {
  return vlink_BuiltInRaw_initialize_ex(sample, RTI_TRUE, RTI_TRUE);
}
RTIBool vlink_BuiltInRaw_initialize_w_params(vlink_BuiltInRaw *sample,
                                                const struct DDS_TypeAllocationParams_t *allocParams) {
  void *buffer = NULL;
  if (buffer) {
  } /* To avoid warnings */

  if (sample == NULL) {
    return RTI_FALSE;
  }
  if (allocParams == NULL) {
    return RTI_FALSE;
  }

  sample->id = 0ull;

  if (allocParams->allocate_memory) {
    if (!DDS_OctetSeq_initialize(&sample->data)) {
      return RTI_FALSE;
    }
    if (!DDS_OctetSeq_set_absolute_maximum(&sample->data, RTIXCdrLong_MAX)) {
      return RTI_FALSE;
    }
    if (!DDS_OctetSeq_set_maximum(&sample->data, (0))) {
      return RTI_FALSE;
    }
  } else {
    if (!DDS_OctetSeq_set_length(&sample->data, 0)) {
      return RTI_FALSE;
    }
  }
  return RTI_TRUE;
}
RTIBool vlink_BuiltInRaw_initialize_ex(vlink_BuiltInRaw *sample, RTIBool allocatePointers,
                                          RTIBool allocateMemory) {
  struct DDS_TypeAllocationParams_t allocParams = DDS_TYPE_ALLOCATION_PARAMS_DEFAULT;

  allocParams.allocate_pointers = (DDS_Boolean)allocatePointers;
  allocParams.allocate_memory = (DDS_Boolean)allocateMemory;

  return vlink_BuiltInRaw_initialize_w_params(sample, &allocParams);
}

RTIBool vlink_BuiltInRaw_finalize_w_return(vlink_BuiltInRaw *sample) {
  vlink_BuiltInRaw_finalize_ex(sample, RTI_TRUE);

  return RTI_TRUE;
}

void vlink_BuiltInRaw_finalize(vlink_BuiltInRaw *sample) { vlink_BuiltInRaw_finalize_ex(sample, RTI_TRUE); }

void vlink_BuiltInRaw_finalize_ex(vlink_BuiltInRaw *sample, RTIBool deletePointers) {
  struct DDS_TypeDeallocationParams_t deallocParams = DDS_TYPE_DEALLOCATION_PARAMS_DEFAULT;

  if (sample == NULL) {
    return;
  }

  deallocParams.delete_pointers = (DDS_Boolean)deletePointers;

  vlink_BuiltInRaw_finalize_w_params(sample, &deallocParams);
}

void vlink_BuiltInRaw_finalize_w_params(vlink_BuiltInRaw *sample,
                                           const struct DDS_TypeDeallocationParams_t *deallocParams) {
  if (sample == NULL) {
    return;
  }

  if (deallocParams == NULL) {
    return;
  }

  RTIOsapiUtility_unusedReturnValue(DDS_OctetSeq_finalize(&sample->data), DDS_Boolean);
}

void vlink_BuiltInRaw_finalize_optional_members(vlink_BuiltInRaw *sample, RTIBool deletePointers) {
  struct DDS_TypeDeallocationParams_t deallocParamsTmp = DDS_TYPE_DEALLOCATION_PARAMS_DEFAULT;
  struct DDS_TypeDeallocationParams_t *deallocParams = &deallocParamsTmp;

  if (sample == NULL) {
    return;
  }
  if (deallocParams) {
  } /* To avoid warnings */

  deallocParamsTmp.delete_pointers = (DDS_Boolean)deletePointers;
  deallocParamsTmp.delete_optional_members = DDS_BOOLEAN_TRUE;
}

RTIBool vlink_BuiltInRaw_copy(vlink_BuiltInRaw *dst, const vlink_BuiltInRaw *src) {
  if (dst == NULL || src == NULL) {
    return RTI_FALSE;
  }

  if (!RTICdrType_copyUnsignedLongLong(&dst->id, &src->id)) {
    return RTI_FALSE;
  }
  if (!DDS_OctetSeq_copy(&dst->data, &src->data)) {
    return RTI_FALSE;
  }

  return RTI_TRUE;
}

/**
 * <<IMPLEMENTATION>>
 *
 * Defines:  TSeq, T
 *
 * Configure and implement 'vlink_BuiltInRaw' sequence class.
 */
#define T vlink_BuiltInRaw
#define TSeq vlink_BuiltInRawSeq

#define T_initialize_w_params vlink_BuiltInRaw_initialize_w_params

#define T_finalize_w_params vlink_BuiltInRaw_finalize_w_params
#define T_copy vlink_BuiltInRaw_copy

#ifndef NDDS_STANDALONE_TYPE
#include "dds_c/generic/dds_c_sequence_TSeq.gen"
#else
#include "dds_c_sequence_TSeq.gen"
#endif

#undef T_copy
#undef T_finalize_w_params

#undef T_initialize_w_params

#undef TSeq
#undef T

// NOLINTEND
