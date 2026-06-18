/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl
using RTI Code Generator (rtiddsgen) version 4.3.0.
The rtiddsgen tool is part of the RTI Connext DDS distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the Code Generator User's Manual.
*/

// NOLINTBEGIN

#include "BuiltInRawSupport.h"

#include "BuiltInRawPlugin.h"

/* ========================================================================= */
/**
<<IMPLEMENTATION>>

Defines:   TData,
TDataWriter,
TDataReader,
TTypeSupport

Configure and implement 'vlink_BuiltInRaw' support classes.

Note: Only the #defined classes get defined
*/

/* ----------------------------------------------------------------- */
/* DDSDataWriter
 */

/**
<<IMPLEMENTATION >>

Defines:   TDataWriter, TData
*/

/* Requires */
#define TTYPENAME vlink_BuiltInRawTYPENAME

/* Defines */
#define TDataWriter vlink_BuiltInRawDataWriter
#define TData vlink_BuiltInRaw

#define RTI_ENABLE_TDATAWRITER_DATA_CONSTRUCTOR_METHODS
#include "dds_c/generic/dds_c_data_TDataWriter.gen"
#undef RTI_ENABLE_TDATAWRITER_DATA_CONSTRUCTOR_METHODS

#undef TDataWriter
#undef TData

#undef TTYPENAME

/* ----------------------------------------------------------------- */
/* DDSDataReader
 */

/**
<<IMPLEMENTATION >>

Defines:   TDataReader, TDataSeq, TData
*/

/* Requires */
#define TTYPENAME vlink_BuiltInRawTYPENAME

/* Defines */
#define TDataReader vlink_BuiltInRawDataReader
#define TDataSeq vlink_BuiltInRawSeq
#define TData vlink_BuiltInRaw

#define RTI_ENABLE_TDATAREADER_DATA_CONSISTENCY_CHECK_METHOD
#include "dds_c/generic/dds_c_data_TDataReader.gen"
#undef RTI_ENABLE_TDATAREADER_DATA_CONSISTENCY_CHECK_METHOD

#undef TDataReader
#undef TDataSeq
#undef TData

#undef TTYPENAME

/* ----------------------------------------------------------------- */
/* TypeSupport

<<IMPLEMENTATION >>

Requires:  TTYPENAME,
TPlugin_new
TPlugin_delete
Defines:   TTypeSupport, TData, TDataReader, TDataWriter
*/

/* Requires */
#define TTYPENAME vlink_BuiltInRawTYPENAME
#define TPlugin_new vlink_BuiltInRawPlugin_new
#define TPlugin_delete vlink_BuiltInRawPlugin_delete

/* Defines */
#define TTypeSupport vlink_BuiltInRawTypeSupport
#define TData vlink_BuiltInRaw
#define TDataReader vlink_BuiltInRawDataReader
#define TDataWriter vlink_BuiltInRawDataWriter
#define TGENERATE_SER_CODE
#ifndef NDDS_STANDALONE_TYPE
#define TGENERATE_TYPECODE
#endif

#include "dds_c/generic/dds_c_data_TTypeSupport.gen"

#undef TTypeSupport
#undef TData
#undef TDataReader
#undef TDataWriter
#ifndef NDDS_STANDALONE_TYPE
#undef TGENERATE_TYPECODE
#endif
#undef TGENERATE_SER_CODE
#undef TTYPENAME
#undef TPlugin_new
#undef TPlugin_delete

// NOLINTEND
