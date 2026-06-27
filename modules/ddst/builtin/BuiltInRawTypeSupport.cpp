// NOLINTBEGIN

#include "BuiltInRawTypeSupport.h"

USING_TRAVODDS_NAMESPACE

#define VLINK_ENABLE_DDST_HACK

#ifdef VLINK_ENABLE_DDST_HACK
#include "./thirdparty/access_private.h"

ACCESS_PRIVATE_FIELD(CdrDeserializer, SerializedBuffer*, curProcessMsgBlock_);
#endif

#ifdef __cplusplus
extern "C" {
#endif

vlink::BuiltInRawTypeSupport* vlink::BuiltInRawTypeSupport::get_instance() {
  static vlink::BuiltInRawTypeSupport instance;
  return &instance;
}

void* vlink::BuiltInRawTypeSupport::create_data() { return new vlink::BuiltInRaw(); }

void vlink::BuiltInRawTypeSupport::delete_data(void* data) { delete static_cast<vlink::BuiltInRaw*>(data); }

int vlink::BuiltInRawTypeSupport::copy_data(void* dst, void* src) {
  vlink::BuiltInRaw* dstData = static_cast<vlink::BuiltInRaw*>(dst);
  vlink::BuiltInRaw* srcData = static_cast<vlink::BuiltInRaw*>(src);
  *dstData = *srcData;
  return 0;
}

unsigned int vlink::BuiltInRawTypeSupport::get_serialized_data_size(void* data, unsigned int currentAlignment) {
  vlink::BuiltInRaw* structData = static_cast<vlink::BuiltInRaw*>(data);
  unsigned int initialAlignment = currentAlignment;
  unsigned int tmpAlignment = 0;
  currentAlignment += CdrSerializer::getBaseTypeSize(sizeof(unsigned long long), currentAlignment);
  currentAlignment += CdrSerializer::getBaseTypeSize(sizeof(uint32_t), currentAlignment);
  currentAlignment +=
      CdrSerializer::getBaseTypeArraySize(sizeof(unsigned char), structData->data.size(), currentAlignment);
  return currentAlignment - initialAlignment;
}

unsigned int vlink::BuiltInRawTypeSupport::get_max_serialized_data_size(void* data, unsigned int currentAlignment) {
  return LENGTH_UNLIMITED;
}

int vlink::BuiltInRawTypeSupport::serialize_data(void* data, CdrSerializer* cdr, int endian) {
  vlink::BuiltInRaw* structData = static_cast<vlink::BuiltInRaw*>(data);
  uint32_t tmpLength = 0;

  if (!cdr->serializeBaseType(structData->id)) {
    fprintf(stderr, "Serialization failed for field: structData->id\n");
    return -1;
  }

  tmpLength = static_cast<uint32_t>(structData->data.size());

  if (!cdr->serializeBaseType(tmpLength)) {
    fprintf(stderr, "Serialization length failed for field: structData->data\n");
    return -1;
  }


  if (!cdr->serializeBaseTypeArray(structData->data.data(), structData->data.size())) {
    fprintf(stderr, "Serialization failed for field: structData->data\n");
    return -1;
  }

  return 0;
}

int vlink::BuiltInRawTypeSupport::deserialize_data(void* data, CdrDeserializer* cdr, int endian) {
  vlink::BuiltInRaw* structData = static_cast<vlink::BuiltInRaw*>(data);
  unsigned int tmpLength = 0;
  char tmpCharEnum = 0;
  short tmpShortEnum = 0;
  int tmpIntEnum = 0;

  if (!cdr->deserializeBaseType(structData->id)) {
    fprintf(stderr, "Deserialization failed for field: structData->id\n");
    return -1;
  }


  if (!cdr->deserializeBaseType(tmpLength)) {
    fprintf(stderr, "Deserialization length failed for field: structData->data\n");
    return -1;
  }

  auto* cur_msg = access_private::curProcessMsgBlock_(*cdr);
  static constexpr size_t skip_size = sizeof(unsigned long long) + sizeof(unsigned int);

  if (cur_msg->buffer_size < skip_size) {
    fprintf(stderr, "Deserialization skip_size failed for field: structData->data\n");
    return -1;
  }

  structData->data =
      Bytes::shallow_copy(reinterpret_cast<uint8_t*>(cur_msg->buffer) + skip_size, cur_msg->buffer_size - skip_size);
  return 0;
}

TypeObject* vlink::BuiltInRawTypeSupport::get_typeobject() { return nullptr; }

int vlink::BuiltInRawTypeSupport::serialize_key(void* data, CdrSerializer* cdr, int endian) {
  vlink::BuiltInRaw* structData = static_cast<vlink::BuiltInRaw*>(data);
  bool memberHasKey = false;
  return 0;
}

int vlink::BuiltInRawTypeSupport::MakeKey(const void* data, InstanceHandle_t& iHandle, bool forceMd5) {
  unsigned int serializedSize = get_serialized_data_size((void*)data, 0);
  SerializedBuffer buffer;
  buffer.buffer_size = serializedSize;
  buffer.buffer = new char[buffer.buffer_size];
  CdrSerializer cdr(&buffer);
  ReturnCode_t ret = get_instancehandle((void*)data, &cdr, iHandle, forceMd5);
  delete[] buffer.buffer;
  return ret == RETCODE_OK ? 0 : -1;
}

ReturnCode_t vlink::BuiltInRawTypeSupport::get_instancehandle(void* data, CdrSerializer* cdr, InstanceHandle_t& iHandle,
                                                              bool forceMd5) {
  if (!has_key()) {
    iHandle = HANDLE_NIL;
    return RETCODE_OK;
  }

  int ret = serialize_key(data, cdr, forceMd5);

  if (ret != 0) {
    fprintf(stderr, "Failed to serialize key.\n");
    return RETCODE_ERROR;
  }


  if (!cdr->getKeyHash((char*)&iHandle, forceMd5)) {
    fprintf(stderr, "Failed to get key hash\n");
    return RETCODE_ERROR;
  }

  return RETCODE_OK;
}

bool vlink::BuiltInRawTypeSupport::has_key() { return false; }

const char* vlink::BuiltInRawTypeSupport::get_typename() { return "vlink::dds_::BuiltInRaw_"; }

#ifdef __cplusplus
}
#endif

// NOLINTEND