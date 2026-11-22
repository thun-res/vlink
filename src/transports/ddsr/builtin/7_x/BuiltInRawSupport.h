/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl
using RTI Code Generator (rtiddsgen) version 4.3.0.
The rtiddsgen tool is part of the RTI Connext DDS distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the Code Generator User's Manual.
*/

// NOLINTBEGIN

#ifndef BuiltInRawSupport_1859968709_h
#define BuiltInRawSupport_1859968709_h

/* Uses */
#include "BuiltInRaw.h"

#ifndef ndds_c_h
#include "ndds/ndds_c.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if (defined(RTI_WIN32) || defined(RTI_WINCE) || defined(RTI_INTIME)) && defined(NDDS_USER_DLL_EXPORT)

#endif

/* ========================================================================= */
/**
Uses:     T

Defines:  TTypeSupport, TDataWriter, TDataReader

Organized using the well-documented "Generics Pattern" for
implementing generics in C and C++.
*/

#if (defined(RTI_WIN32) || defined(RTI_WINCE) || defined(RTI_INTIME)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, start exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport __declspec(dllexport)

#endif

DDS_TYPESUPPORT_C(vlink_BuiltInRawTypeSupport, vlink_BuiltInRaw);
DDS_DATAWRITER_WITH_DATA_CONSTRUCTOR_METHODS_C(vlink_BuiltInRawDataWriter, vlink_BuiltInRaw);
DDS_DATAREADER_C(vlink_BuiltInRawDataReader, vlink_BuiltInRawSeq, vlink_BuiltInRaw);

#if (defined(RTI_WIN32) || defined(RTI_WINCE) || defined(RTI_INTIME)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, stop exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport
#endif

#ifdef __cplusplus
}
#endif

#endif /* BuiltInRawSupport_1859968709_h */

// NOLINTEND
