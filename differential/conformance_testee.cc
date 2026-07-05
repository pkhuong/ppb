/*
 * PPB conformance testee: speaks the protobuf conformance wire protocol
 * (length-prefixed ConformanceRequest / ConformanceResponse pairs) over
 * stdin/stdout.  Decodes protobuf_payload with the protoc-gen-ppb-generated
 * ppb::reader / reflection sink, re-serializes with libprotobuf, and writes
 * the response back.  Only proto3 / PROTOBUF output is handled; everything
 * else is skipped.
 */

#include "conformance/conformance.pb.h"
#include "conformance_loop.hpp"
#include "google/protobuf/test_messages_proto3.pb.h"
#include "google/protobuf/test_messages_proto3.ppb.reflect.hpp"

#include <span>
#include <string>

namespace
{

constexpr char kProto3Name[] = "protobuf_test_messages.proto3.TestAllTypesProto3";

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

    /* Only proto3 TestAllTypesProto3 is handled. */
    if (request.message_type() != kProto3Name)
    {
        response.set_skipped("unsupported message type");
        return response;
    }

    /* Only protobuf wire payload is decoded by this testee. */
    if (request.payload_case() != conformance::ConformanceRequest::kProtobufPayload)
    {
        response.set_skipped("only protobuf wire payload is supported");
        return response;
    }

    ::protobuf_test_messages::proto3::TestAllTypesProto3 message;
    const std::string &payload = request.protobuf_payload();
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte *>(payload.data()), payload.size());

    ppb_error err = ppb_gen::protobuf_test_messages::proto3::TestAllTypesProto3::parse_into(bytes, &message);

    if (err != PPB_OK)
    {
        /* Malformed inputs are an expected conformance test category. */
        response.set_parse_error("ppb decode failed");
        return response;
    }

    if (!message.IsInitialized())
    {
        response.set_parse_error("missing required field");
        return response;
    }

    if (!message.SerializeToString(response.mutable_protobuf_payload()))
    {
        response.set_serialize_error("libprotobuf re-serialization failed");
        return response;
    }

    return response;
}

}  // namespace

int
main()
{
    return ppb_conformance::run_loop(run_test);
}
