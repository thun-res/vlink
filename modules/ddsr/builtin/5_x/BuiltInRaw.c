/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl using "rtiddsgen".
The rtiddsgen tool is part of the RTI Connext distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the RTI Connext manual.
*/

// NOLINTBEGIN

#ifndef NDDS_STANDALONE_TYPE
#ifndef ndds_c_h
#include "ndds/ndds_c.h"
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

/* ========================================================================= */
const char* vlink_BuiltInRawTYPENAME = "vlink::dds_::BuiltInRaw_";

DDS_TypeCode* vlink_BuiltInRaw_get_typecode(void) {
  static RTIBool is_initialized = RTI_FALSE;

  static DDS_TypeCode vlink_BuiltInRaw_g_tc_data_sequence = DDS_INITIALIZE_SEQUENCE_TYPECODE(RTI_INT32_MAX, NULL);
  static DDS_TypeCode_Member vlink_BuiltInRaw_g_tc_members[2] = {

      {
          (char*)"id", /* Member name */
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
          NULL /* Ignored */
      },
      {
          (char*)"data", /* Member name */
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
          NULL /* Ignored */
      }};

  static DDS_TypeCode vlink_BuiltInRaw_g_tc = {{
      DDS_TK_STRUCT,                    /* Kind */
      DDS_BOOLEAN_FALSE,                /* Ignored */
      -1,                               /*Ignored*/
      (char*)"vlink::dds_::BuiltInRaw_",   /* Name */
      NULL,                             /* Ignored */
      0,                                /* Ignored */
      0,                                /* Ignored */
      NULL,                             /* Ignored */
      2,                                /* Number of members */
      vlink_BuiltInRaw_g_tc_members, /* Members */
      DDS_VM_NONE                       /* Ignored */
  }};                                   /* Type code for vlink_BuiltInRaw*/

  if (is_initialized) {
    return &vlink_BuiltInRaw_g_tc;
  }

  vlink_BuiltInRaw_g_tc_data_sequence._data._typeCode = (RTICdrTypeCode*)&DDS_g_tc_octet;

  vlink_BuiltInRaw_g_tc_members[0]._representation._typeCode = (RTICdrTypeCode*)&DDS_g_tc_ulonglong;

  vlink_BuiltInRaw_g_tc_members[1]._representation._typeCode =
      (RTICdrTypeCode*)&vlink_BuiltInRaw_g_tc_data_sequence;

  is_initialized = RTI_TRUE;

  return &vlink_BuiltInRaw_g_tc;
}

RTIBool vlink_BuiltInRaw_initialize(vlink_BuiltInRaw* sample) {
  return vlink_BuiltInRaw_initialize_ex(sample, RTI_TRUE, RTI_TRUE);
}

RTIBool vlink_BuiltInRaw_initialize_ex(vlink_BuiltInRaw* sample, RTIBool allocatePointers,
                                          RTIBool allocateMemory) {
  struct DDS_TypeAllocationParams_t allocParams = DDS_TYPE_ALLOCATION_PARAMS_DEFAULT;

  allocParams.allocate_pointers = (DDS_Boolean)allocatePointers;
  allocParams.allocate_memory = (DDS_Boolean)allocateMemory;

  return vlink_BuiltInRaw_initialize_w_params(sample, &allocParams);
}

RTIBool vlink_BuiltInRaw_initialize_w_params(vlink_BuiltInRaw* sample,
                                                const struct DDS_TypeAllocationParams_t* allocParams) {
  void* buffer = NULL;
  if (buffer) {
  } /* To avoid warnings */

  if (sample == NULL) {
    return RTI_FALSE;
  }
  if (allocParams == NULL) {
    return RTI_FALSE;
  }

  if (!RTICdrType_initUnsignedLongLong(&sample->id)) {
    return RTI_FALSE;
  }

  if (allocParams->allocate_memory) {
    DDS_OctetSeq_initialize(&sample->data);
    DDS_OctetSeq_set_absolute_maximum(&sample->data, RTI_INT32_MAX);
    if (!DDS_OctetSeq_set_maximum(&sample->data, (0))) {
      return RTI_FALSE;
    }
  } else {
    DDS_OctetSeq_set_length(&sample->data, 0);
  }
  return RTI_TRUE;
}

void vlink_BuiltInRaw_finalize(vlink_BuiltInRaw* sample) { vlink_BuiltInRaw_finalize_ex(sample, RTI_TRUE); }

void vlink_BuiltInRaw_finalize_ex(vlink_BuiltInRaw* sample, RTIBool deletePointers) {
  struct DDS_TypeDeallocationParams_t deallocParams = DDS_TYPE_DEALLOCATION_PARAMS_DEFAULT;

  if (sample == NULL) {
    return;
  }

  deallocParams.delete_pointers = (DDS_Boolean)deletePointers;

  vlink_BuiltInRaw_finalize_w_params(sample, &deallocParams);
}

void vlink_BuiltInRaw_finalize_w_params(vlink_BuiltInRaw* sample,
                                           const struct DDS_TypeDeallocationParams_t* deallocParams) {
  if (sample == NULL) {
    return;
  }

  if (deallocParams == NULL) {
    return;
  }

  DDS_OctetSeq_finalize(&sample->data);
}

void vlink_BuiltInRaw_finalize_optional_members(vlink_BuiltInRaw* sample, RTIBool deletePointers) {
  struct DDS_TypeDeallocationParams_t deallocParamsTmp = DDS_TYPE_DEALLOCATION_PARAMS_DEFAULT;
  struct DDS_TypeDeallocationParams_t* deallocParams = &deallocParamsTmp;

  if (sample == NULL) {
    return;
  }
  if (deallocParams) {
  } /* To avoid warnings */

  deallocParamsTmp.delete_pointers = (DDS_Boolean)deletePointers;
  deallocParamsTmp.delete_optional_members = DDS_BOOLEAN_TRUE;
}

RTIBool vlink_BuiltInRaw_copy(vlink_BuiltInRaw* dst, const vlink_BuiltInRaw* src) {
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
