/*
 * PPB proto2 conformance testee: speaks the protobuf conformance wire protocol
 * (length-prefixed ConformanceRequest / ConformanceResponse pairs) over
 * stdin/stdout.  Decodes protobuf_payload with the protoc-gen-ppb-generated
 * reflection sink, re-serializes with libprotobuf via reflection, and writes
 * the response back.
 *
 * Messages are created dynamically from the generated descriptor pool to avoid
 * including test_messages_proto2.pb.h, which declares accessor methods named
 * after C++20 keywords (concept, requires) that break compilation under
 * -std=c++20.  The generated .pb.cc self-registers its descriptors so the pool
 * is populated at startup without requiring the .pb.h.
 */

#include "conformance_loop.hpp"
#include "google/protobuf/test_messages_proto2.ppb.reflect.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <memory>
#include <span>
#include <string>

namespace
{

constexpr char kProto2Name[] = "protobuf_test_messages.proto2.TestAllTypesProto2";
constexpr char kProto2RequiredName[] = "protobuf_test_messages.proto2.TestAllRequiredTypesProto2";

template <typename ParseInto>
conformance::ConformanceResponse
decode(const char *type_name, const std::string &payload, ParseInto parse_into_fn)
{
    conformance::ConformanceResponse response;

    const ::google::protobuf::Descriptor *desc =
        ::google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(type_name);

    if (desc == nullptr)
    {
        response.set_runtime_error("descriptor not found");
        return response;
    }

    const ::google::protobuf::Message *prototype =
        ::google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);

    if (prototype == nullptr)
    {
        response.set_runtime_error("message prototype not found");
        return response;
    }

    std::unique_ptr<::google::protobuf::Message> message(prototype->New());

    std::span<const std::byte> bytes(reinterpret_cast<const std::byte *>(payload.data()), payload.size());

    ppb_error err = parse_into_fn(bytes, message.get());

    if (err != PPB_OK)
    {
        response.set_parse_error("ppb decode failed");
        return response;
    }

    if (!message->IsInitialized())
    {
        response.set_parse_error("missing required field");
        return response;
    }

    if (!message->SerializeToString(response.mutable_protobuf_payload()))
    {
        response.set_serialize_error("libprotobuf re-serialization failed");
        return response;
    }

    return response;
}

conformance::ConformanceResponse
run_test(const conformance::ConformanceRequest &request)
{
    conformance::ConformanceResponse response;

    /* Only PROTOBUF output is implemented. */
    if (request.requested_output_format() != conformance::PROTOBUF)
    {
        response.set_skipped("only PROTOBUF output is supported");
        return response;
    }

    /* Only protobuf wire payload is decoded by this testee. */
    if (request.payload_case() != conformance::ConformanceRequest::kProtobufPayload)
    {
        response.set_skipped("only protobuf wire payload is supported");
        return response;
    }

    const std::string &payload = request.protobuf_payload();
    const std::string &msg_type = request.message_type();

    if (msg_type == kProto2Name)
    {
        return decode(kProto2Name, payload,
            [](std::span<const std::byte> b, ::google::protobuf::Message *m) -> ppb_error
            { return ppb_gen::protobuf_test_messages::proto2::TestAllTypesProto2::parse_into(b, m); });
    }

    if (msg_type == kProto2RequiredName)
    {
        return decode(kProto2RequiredName, payload,
            [](std::span<const std::byte> b, ::google::protobuf::Message *m) -> ppb_error
            {
                return ppb_gen::protobuf_test_messages::proto2::TestAllRequiredTypesProto2::parse_into(b, m);
            });
    }

    response.set_skipped("unsupported message type");
    return response;
}

}  // namespace

int
main()
{
    return ppb_conformance::run_loop(run_test);
}
