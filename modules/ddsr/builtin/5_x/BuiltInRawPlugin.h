/*
WARNING: THIS FILE IS AUTO-GENERATED. DO NOT MODIFY.

This file was generated from BuiltInRaw.idl using "rtiddsgen".
The rtiddsgen tool is part of the RTI Connext distribution.
For more information, type 'rtiddsgen -help' at a command shell
or consult the RTI Connext manual.
*/

// NOLINTBEGIN

#ifndef BuiltInRawPlugin_1859968709_h
#define BuiltInRawPlugin_1859968709_h

#include "BuiltInRaw.h"

struct RTICdrStream;

#ifndef pres_typePlugin_h
#include "pres/pres_typePlugin.h"
#endif

#if (defined(RTI_WIN32) || defined(RTI_WINCE)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, start exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport __declspec(dllexport)
#endif

#define vlink_BuiltInRawPlugin_get_sample PRESTypePluginDefaultEndpointData_getSample
#define vlink_BuiltInRawPlugin_get_buffer PRESTypePluginDefaultEndpointData_getBuffer
#define vlink_BuiltInRawPlugin_return_buffer PRESTypePluginDefaultEndpointData_returnBuffer

#define vlink_BuiltInRawPlugin_create_sample PRESTypePluginDefaultEndpointData_createSample
#define vlink_BuiltInRawPlugin_destroy_sample PRESTypePluginDefaultEndpointData_deleteSample

/* --------------------------------------------------------------------------------------
Support functions:
* -------------------------------------------------------------------------------------- */

NDDSUSERDllExport extern vlink_BuiltInRaw *vlink_BuiltInRawPluginSupport_create_data_w_params(
    const struct DDS_TypeAllocationParams_t *alloc_params);

NDDSUSERDllExport extern vlink_BuiltInRaw *vlink_BuiltInRawPluginSupport_create_data_ex(
    RTIBool allocate_pointers);

NDDSUSERDllExport extern vlink_BuiltInRaw *vlink_BuiltInRawPluginSupport_create_data(void);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPluginSupport_copy_data(vlink_BuiltInRaw *out,
                                                                            const vlink_BuiltInRaw *in);

NDDSUSERDllExport extern void vlink_BuiltInRawPluginSupport_destroy_data_w_params(
    vlink_BuiltInRaw *sample, const struct DDS_TypeDeallocationParams_t *dealloc_params);

NDDSUSERDllExport extern void vlink_BuiltInRawPluginSupport_destroy_data_ex(vlink_BuiltInRaw *sample,
                                                                               RTIBool deallocate_pointers);

NDDSUSERDllExport extern void vlink_BuiltInRawPluginSupport_destroy_data(vlink_BuiltInRaw *sample);

NDDSUSERDllExport extern void vlink_BuiltInRawPluginSupport_print_data(const vlink_BuiltInRaw *sample,
                                                                          const char *desc, unsigned int indent);

/* ----------------------------------------------------------------------------
Callback functions:
* ---------------------------------------------------------------------------- */

NDDSUSERDllExport extern PRESTypePluginParticipantData vlink_BuiltInRawPlugin_on_participant_attached(
    void *registration_data, const struct PRESTypePluginParticipantInfo *participant_info,
    RTIBool top_level_registration, void *container_plugin_context, RTICdrTypeCode *typeCode);

NDDSUSERDllExport extern void vlink_BuiltInRawPlugin_on_participant_detached(
    PRESTypePluginParticipantData participant_data);

NDDSUSERDllExport extern PRESTypePluginEndpointData vlink_BuiltInRawPlugin_on_endpoint_attached(
    PRESTypePluginParticipantData participant_data, const struct PRESTypePluginEndpointInfo *endpoint_info,
    RTIBool top_level_registration, void *container_plugin_context);

NDDSUSERDllExport extern void vlink_BuiltInRawPlugin_on_endpoint_detached(PRESTypePluginEndpointData endpoint_data);

NDDSUSERDllExport extern void vlink_BuiltInRawPlugin_return_sample(PRESTypePluginEndpointData endpoint_data,
                                                                      vlink_BuiltInRaw *sample, void *handle);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_copy_sample(PRESTypePluginEndpointData endpoint_data,
                                                                       vlink_BuiltInRaw *out,
                                                                       const vlink_BuiltInRaw *in);

/* ----------------------------------------------------------------------------
(De)Serialize functions:
* ------------------------------------------------------------------------- */

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_serialize(
    PRESTypePluginEndpointData endpoint_data, const vlink_BuiltInRaw *sample, struct RTICdrStream *stream,
    RTIBool serialize_encapsulation, RTIEncapsulationId encapsulation_id, RTIBool serialize_sample,
    void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_deserialize_sample(
    PRESTypePluginEndpointData endpoint_data, vlink_BuiltInRaw *sample, struct RTICdrStream *stream,
    RTIBool deserialize_encapsulation, RTIBool deserialize_sample, void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_serialize_to_cdr_buffer(char *buffer, unsigned int *length,
                                                                                   const vlink_BuiltInRaw *sample);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_deserialize(
    PRESTypePluginEndpointData endpoint_data, vlink_BuiltInRaw **sample, RTIBool *drop_sample,
    struct RTICdrStream *stream, RTIBool deserialize_encapsulation, RTIBool deserialize_sample,
    void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_deserialize_from_cdr_buffer(vlink_BuiltInRaw *sample,
                                                                                       const char *buffer,
                                                                                       unsigned int length);
NDDSUSERDllExport extern DDS_ReturnCode_t vlink_BuiltInRawPlugin_data_to_string(
    const vlink_BuiltInRaw *sample, char *str, DDS_UnsignedLong *str_size,
    const struct DDS_PrintFormatProperty *property);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_skip(PRESTypePluginEndpointData endpoint_data,
                                                                struct RTICdrStream *stream, RTIBool skip_encapsulation,
                                                                RTIBool skip_sample, void *endpoint_plugin_qos);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_sample_max_size_ex(
    PRESTypePluginEndpointData endpoint_data, RTIBool *overflow, RTIBool include_encapsulation,
    RTIEncapsulationId encapsulation_id, unsigned int current_alignment);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_sample_max_size(
    PRESTypePluginEndpointData endpoint_data, RTIBool include_encapsulation, RTIEncapsulationId encapsulation_id,
    unsigned int current_alignment);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_sample_min_size(
    PRESTypePluginEndpointData endpoint_data, RTIBool include_encapsulation, RTIEncapsulationId encapsulation_id,
    unsigned int current_alignment);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_sample_size(
    PRESTypePluginEndpointData endpoint_data, RTIBool include_encapsulation, RTIEncapsulationId encapsulation_id,
    unsigned int current_alignment, const vlink_BuiltInRaw *sample);

/* --------------------------------------------------------------------------------------
Key Management functions:
* -------------------------------------------------------------------------------------- */
NDDSUSERDllExport extern PRESTypePluginKeyKind vlink_BuiltInRawPlugin_get_key_kind(void);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_key_max_size_ex(
    PRESTypePluginEndpointData endpoint_data, RTIBool *overflow, RTIBool include_encapsulation,
    RTIEncapsulationId encapsulation_id, unsigned int current_alignment);

NDDSUSERDllExport extern unsigned int vlink_BuiltInRawPlugin_get_serialized_key_max_size(
    PRESTypePluginEndpointData endpoint_data, RTIBool include_encapsulation, RTIEncapsulationId encapsulation_id,
    unsigned int current_alignment);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_serialize_key(
    PRESTypePluginEndpointData endpoint_data, const vlink_BuiltInRaw *sample, struct RTICdrStream *stream,
    RTIBool serialize_encapsulation, RTIEncapsulationId encapsulation_id, RTIBool serialize_key,
    void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_deserialize_key_sample(
    PRESTypePluginEndpointData endpoint_data, vlink_BuiltInRaw *sample, struct RTICdrStream *stream,
    RTIBool deserialize_encapsulation, RTIBool deserialize_key, void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_deserialize_key(
    PRESTypePluginEndpointData endpoint_data, vlink_BuiltInRaw **sample, RTIBool *drop_sample,
    struct RTICdrStream *stream, RTIBool deserialize_encapsulation, RTIBool deserialize_key, void *endpoint_plugin_qos);

NDDSUSERDllExport extern RTIBool vlink_BuiltInRawPlugin_serialized_sample_to_key(
    PRESTypePluginEndpointData endpoint_data, vlink_BuiltInRaw *sample, struct RTICdrStream *stream,
    RTIBool deserialize_encapsulation, RTIBool deserialize_key, void *endpoint_plugin_qos);

/* Plugin Functions */
NDDSUSERDllExport extern struct PRESTypePlugin *vlink_BuiltInRawPlugin_new(void);

NDDSUSERDllExport extern void vlink_BuiltInRawPlugin_delete(struct PRESTypePlugin *);

#if (defined(RTI_WIN32) || defined(RTI_WINCE)) && defined(NDDS_USER_DLL_EXPORT)
/* If the code is building on Windows, stop exporting symbols.
 */
#undef NDDSUSERDllExport
#define NDDSUSERDllExport
#endif

#endif /* BuiltInRawPlugin_1859968709_h */

// NOLINTEND
