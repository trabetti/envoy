#include "common/protobuf/utility.h"

#include "common/common/assert.h"
#include "common/filesystem/filesystem_impl.h"
#include "common/json/json_loader.h"
#include "common/protobuf/protobuf.h"

#include "fmt/format.h"

namespace Envoy {

MissingFieldException::MissingFieldException(const std::string& field_name,
                                             const Protobuf::Message& message)
    : EnvoyException(
          fmt::format("Field '{}' is missing in: {}", field_name, message.DebugString())) {}

ProtoValidationException::ProtoValidationException(const std::string& validation_error,
                                                   const Protobuf::Message& message)
    : EnvoyException(fmt::format("Proto constraint validation failed ({}): {}", validation_error,
                                 message.DebugString())) {
  ENVOY_LOG_MISC(debug, "Proto validation error; throwing {}", what());
}

void MessageUtil::loadFromJson(const std::string& json, Protobuf::Message& message) {
  const auto status = Protobuf::util::JsonStringToMessage(json, &message);
  if (!status.ok()) {
    throw EnvoyException("Unable to parse JSON as proto (" + status.ToString() + "): " + json);
  }
}

void MessageUtil::loadFromYaml(const std::string& yaml, Protobuf::Message& message) {
  const std::string json = Json::Factory::loadFromYamlString(yaml)->asJsonString();
  loadFromJson(json, message);
}

void MessageUtil::loadFromFile(const std::string& path, Protobuf::Message& message) {
  const std::string contents = Filesystem::fileReadToEnd(path);
  // If the filename ends with .pb, attempt to parse it as a binary proto.
  if (StringUtil::endsWith(path, ".pb")) {
    // Attempt to parse the binary format.
    if (message.ParseFromString(contents)) {
      return;
    }
    throw EnvoyException("Unable to parse file \"" + path + "\" as a binary protobuf (type " +
                         message.GetTypeName() + ")");
  }
  // If the filename ends with .pb_text, attempt to parse it as a text proto.
  if (StringUtil::endsWith(path, ".pb_text")) {
    if (Protobuf::TextFormat::ParseFromString(contents, &message)) {
      return;
    }
    throw EnvoyException("Unable to parse file \"" + path + "\" as a text protobuf (type " +
                         message.GetTypeName() + ")");
  }
  if (StringUtil::endsWith(path, ".yaml")) {
    loadFromYaml(contents, message);
  } else {
    loadFromJson(contents, message);
  }
}

std::string MessageUtil::getJsonStringFromMessage(const Protobuf::Message& message) {
  Protobuf::util::JsonPrintOptions json_options;
  // By default, proto field names are converted to camelCase when the message is converted to JSON.
  // Setting this option makes debugging easier because it keeps field names consistent in JSON
  // printouts.
  json_options.preserve_proto_field_names = true;
  ProtobufTypes::String json;
  const auto status = Protobuf::util::MessageToJsonString(message, &json, json_options);
  // This should always succeed unless something crash-worthy such as out-of-memory.
  RELEASE_ASSERT(status.ok());
  return json;
}

void MessageUtil::jsonConvert(const Protobuf::Message& source, Protobuf::Message& dest) {
  // TODO(htuch): Consolidate with the inflight cleanups here.
  Protobuf::util::JsonOptions json_options;
  ProtobufTypes::String json;
  const auto status = Protobuf::util::MessageToJsonString(source, &json, json_options);
  // This should always succeed unless something crash-worthy such as out-of-memory.
  RELEASE_ASSERT(status.ok());
  MessageUtil::loadFromJson(json, dest);
}

bool ValueUtil::equal(const ProtobufWkt::Value& v1, const ProtobufWkt::Value& v2) {
  ProtobufWkt::Value::KindCase kind = v1.kind_case();
  if (kind != v2.kind_case()) {
    return false;
  }

  switch (kind) {
  case ProtobufWkt::Value::kNullValue:
    return true;

  case ProtobufWkt::Value::kNumberValue:
    return v1.number_value() == v2.number_value();

  case ProtobufWkt::Value::kStringValue:
    return v1.string_value() == v2.string_value();

  case ProtobufWkt::Value::kBoolValue:
    return v1.bool_value() == v2.bool_value();

  case ProtobufWkt::Value::kStructValue: {
    const ProtobufWkt::Struct& s1 = v1.struct_value();
    const ProtobufWkt::Struct& s2 = v2.struct_value();
    if (s1.fields_size() != s2.fields_size()) {
      return false;
    }
    for (const auto& it1 : s1.fields()) {
      const auto& it2 = s2.fields().find(it1.first);
      if (it2 == s2.fields().end()) {
        return false;
      }

      if (!equal(it1.second, it2->second)) {
        return false;
      }
    }
    return true;
  }

  case ProtobufWkt::Value::kListValue: {
    const ProtobufWkt::ListValue& l1 = v1.list_value();
    const ProtobufWkt::ListValue& l2 = v2.list_value();
    if (l1.values_size() != l2.values_size()) {
      return false;
    }
    for (int i = 0; i < l1.values_size(); i++) {
      if (!equal(l1.values(i), l2.values(i))) {
        return false;
      }
    }
    return true;
  }

  default:
    NOT_REACHED;
  }
}

} // namespace Envoy
