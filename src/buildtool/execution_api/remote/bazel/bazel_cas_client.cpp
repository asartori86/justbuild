#include "src/buildtool/execution_api/remote/bazel/bazel_cas_client.hpp"

#include "grpcpp/grpcpp.h"
#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/common/bazel_types.hpp"
#include "src/buildtool/execution_api/common/execution_common.hpp"
#include "src/buildtool/execution_api/remote/bazel/bazel_client_common.hpp"

namespace {

[[nodiscard]] auto ToResourceName(std::string const& instance_name,
                                  bazel_re::Digest const& digest) noexcept
    -> std::string {
    return fmt::format(
        "{}/blobs/{}/{}", instance_name, digest.hash(), digest.size_bytes());
}

}  // namespace

BazelCasClient::BazelCasClient(std::string const& server,
                               Port port,
                               std::string const& user,
                               std::string const& pwd) noexcept
    : stream_{std::make_unique<ByteStreamClient>(server, port, user, pwd)} {
    stub_ = bazel_re::ContentAddressableStorage::NewStub(
        CreateChannelWithCredentials(server, port, user, pwd));
}

auto BazelCasClient::FindMissingBlobs(
    std::string const& instance_name,
    std::vector<bazel_re::Digest> const& digests) noexcept
    -> std::vector<bazel_re::Digest> {
    return FindMissingBlobs(instance_name, digests.begin(), digests.end());
}

auto BazelCasClient::FindMissingBlobs(
    std::string const& instance_name,
    BlobContainer::DigestList const& digests) noexcept
    -> std::vector<bazel_re::Digest> {
    return FindMissingBlobs(instance_name, digests.begin(), digests.end());
}

auto BazelCasClient::BatchUpdateBlobs(
    std::string const& instance_name,
    std::vector<BazelBlob>::const_iterator const& begin,
    std::vector<BazelBlob>::const_iterator const& end) noexcept
    -> std::vector<bazel_re::Digest> {
    return DoBatchUpdateBlobs(instance_name, begin, end);
}

auto BazelCasClient::BatchUpdateBlobs(
    std::string const& instance_name,
    BlobContainer::iterator const& begin,
    BlobContainer::iterator const& end) noexcept
    -> std::vector<bazel_re::Digest> {
    return DoBatchUpdateBlobs(instance_name, begin, end);
}

auto BazelCasClient::BatchUpdateBlobs(
    std::string const& instance_name,
    BlobContainer::RelatedBlobList::iterator const& begin,
    BlobContainer::RelatedBlobList::iterator const& end) noexcept
    -> std::vector<bazel_re::Digest> {
    return DoBatchUpdateBlobs(instance_name, begin, end);
}

auto BazelCasClient::BatchReadBlobs(
    std::string const& instance_name,
    std::vector<bazel_re::Digest>::const_iterator const& begin,
    std::vector<bazel_re::Digest>::const_iterator const& end) noexcept
    -> std::vector<BazelBlob> {
    auto request =
        CreateRequest<bazel_re::BatchReadBlobsRequest, bazel_re::Digest>(
            instance_name, begin, end);
    grpc::ClientContext context;
    bazel_re::BatchReadBlobsResponse response;
    grpc::Status status = stub_->BatchReadBlobs(&context, request, &response);

    std::vector<BazelBlob> result{};
    if (status.ok()) {
        result =
            ProcessBatchResponse<BazelBlob,
                                 bazel_re::BatchReadBlobsResponse_Response>(
                response,
                [](std::vector<BazelBlob>* v,
                   bazel_re::BatchReadBlobsResponse_Response const& r) {
                    v->emplace_back(r.digest(), r.data());
                });
    }
    else {
        LogStatus(&logger_, LogLevel::Debug, status);
    }

    return result;
}

// NOLINTNEXTLINE(misc-no-recursion)
auto BazelCasClient::GetTree(std::string const& instance_name,
                             bazel_re::Digest const& root_digest,
                             std::int32_t page_size,
                             std::string const& page_token) noexcept
    -> std::vector<bazel_re::Directory> {
    auto request =
        CreateGetTreeRequest(instance_name, root_digest, page_size, page_token);

    grpc::ClientContext context;
    bazel_re::GetTreeResponse response;
    auto stream = stub_->GetTree(&context, request);

    std::vector<bazel_re::Directory> result;
    while (stream->Read(&response)) {
        result = ProcessResponseContents<bazel_re::Directory>(response);
        auto const& next_page_token = response.next_page_token();
        if (!next_page_token.empty()) {
            // recursively call this function with token for next page
            auto next_result =
                GetTree(instance_name, root_digest, page_size, next_page_token);
            std::move(next_result.begin(),
                      next_result.end(),
                      std::back_inserter(result));
        }
    }

    auto status = stream->Finish();
    if (not status.ok()) {
        LogStatus(&logger_, LogLevel::Debug, status);
    }

    return result;
}

auto BazelCasClient::UpdateSingleBlob(std::string const& instance_name,
                                      BazelBlob const& blob) noexcept -> bool {
    thread_local static std::string uuid{};
    if (uuid.empty()) {
        auto id = CreateProcessUniqueId();
        if (not id) {
            logger_.Emit(LogLevel::Debug, "Failed creating process unique id.");
            return false;
        }
        uuid = CreateUUIDVersion4(*id);
    }
    return stream_->Write(fmt::format("{}/uploads/{}/blobs/{}/{}",
                                      instance_name,
                                      uuid,
                                      blob.digest.hash(),
                                      blob.digest.size_bytes()),
                          blob.data);
}

auto BazelCasClient::IncrementalReadSingleBlob(
    std::string const& instance_name,
    bazel_re::Digest const& digest) noexcept
    -> ByteStreamClient::IncrementalReader {
    return stream_->IncrementalRead(ToResourceName(instance_name, digest));
}

auto BazelCasClient::ReadSingleBlob(std::string const& instance_name,
                                    bazel_re::Digest const& digest) noexcept
    -> std::optional<BazelBlob> {
    if (auto data = stream_->Read(ToResourceName(instance_name, digest))) {
        return BazelBlob{ArtifactDigest::Create(*data), std::move(*data)};
    }
    return std::nullopt;
}

template <class T_OutputIter>
auto BazelCasClient::FindMissingBlobs(std::string const& instance_name,
                                      T_OutputIter const& start,
                                      T_OutputIter const& end) noexcept
    -> std::vector<bazel_re::Digest> {
    auto request =
        CreateRequest<bazel_re::FindMissingBlobsRequest, bazel_re::Digest>(
            instance_name, start, end);

    grpc::ClientContext context;
    bazel_re::FindMissingBlobsResponse response;
    grpc::Status status = stub_->FindMissingBlobs(&context, request, &response);

    std::vector<bazel_re::Digest> result{};
    if (status.ok()) {
        result = ProcessResponseContents<bazel_re::Digest>(response);
    }
    else {
        LogStatus(&logger_, LogLevel::Debug, status);
    }

    return result;
}

template <class T_OutputIter>
auto BazelCasClient::DoBatchUpdateBlobs(std::string const& instance_name,
                                        T_OutputIter const& start,
                                        T_OutputIter const& end) noexcept
    -> std::vector<bazel_re::Digest> {
    auto request = CreateUpdateBlobsRequest(instance_name, start, end);

    grpc::ClientContext context;
    bazel_re::BatchUpdateBlobsResponse response;
    grpc::Status status = stub_->BatchUpdateBlobs(&context, request, &response);

    std::vector<bazel_re::Digest> result{};
    if (status.ok()) {
        result =
            ProcessBatchResponse<bazel_re::Digest,
                                 bazel_re::BatchUpdateBlobsResponse_Response>(
                response,
                [](std::vector<bazel_re::Digest>* v,
                   bazel_re::BatchUpdateBlobsResponse_Response const& r) {
                    v->push_back(r.digest());
                });
    }
    else {
        LogStatus(&logger_, LogLevel::Debug, status);
        if (status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {
            logger_.Emit(LogLevel::Debug,
                         "Falling back to single blob transfers");
            auto current = start;
            while (current != end) {
                if (UpdateSingleBlob(instance_name, (*current))) {
                    result.emplace_back((*current).digest);
                }
                ++current;
            }
        }
    }

    return result;
}

namespace detail {

// Getter for request contents (needs specialization, never implemented)
template <class T_Content, class T_Request>
static auto GetRequestContents(T_Request&) noexcept
    -> pb::RepeatedPtrField<T_Content>*;

// Getter for response contents (needs specialization, never implemented)
template <class T_Content, class T_Response>
static auto GetResponseContents(T_Response const&) noexcept
    -> pb::RepeatedPtrField<T_Content> const&;

// Specialization of GetRequestContents for 'FindMissingBlobsRequest'
template <>
auto GetRequestContents<bazel_re::Digest, bazel_re::FindMissingBlobsRequest>(
    bazel_re::FindMissingBlobsRequest& request) noexcept
    -> pb::RepeatedPtrField<bazel_re::Digest>* {
    return request.mutable_blob_digests();
}

// Specialization of GetRequestContents for 'BatchReadBlobsRequest'
template <>
auto GetRequestContents<bazel_re::Digest, bazel_re::BatchReadBlobsRequest>(
    bazel_re::BatchReadBlobsRequest& request) noexcept
    -> pb::RepeatedPtrField<bazel_re::Digest>* {
    return request.mutable_digests();
}

// Specialization of GetResponseContents for 'FindMissingBlobsResponse'
template <>
auto GetResponseContents<bazel_re::Digest, bazel_re::FindMissingBlobsResponse>(
    bazel_re::FindMissingBlobsResponse const& response) noexcept
    -> pb::RepeatedPtrField<bazel_re::Digest> const& {
    return response.missing_blob_digests();
}

// Specialization of GetResponseContents for 'GetTreeResponse'
template <>
auto GetResponseContents<bazel_re::Directory, bazel_re::GetTreeResponse>(
    bazel_re::GetTreeResponse const& response) noexcept
    -> pb::RepeatedPtrField<bazel_re::Directory> const& {
    return response.directories();
}

}  // namespace detail

template <class T_Request, class T_Content, class T_OutputIter>
auto BazelCasClient::CreateRequest(std::string const& instance_name,
                                   T_OutputIter const& start,
                                   T_OutputIter const& end) const noexcept
    -> T_Request {
    T_Request request;
    request.set_instance_name(instance_name);
    std::copy(
        start,
        end,
        pb::back_inserter(detail::GetRequestContents<T_Content>(request)));
    return request;
}

template <class T_OutputIter>
auto BazelCasClient::CreateUpdateBlobsRequest(std::string const& instance_name,
                                              T_OutputIter const& start,
                                              T_OutputIter const& end)
    const noexcept -> bazel_re::BatchUpdateBlobsRequest {
    bazel_re::BatchUpdateBlobsRequest request;
    request.set_instance_name(instance_name);
    std::transform(start,
                   end,
                   pb::back_inserter(request.mutable_requests()),
                   [](BazelBlob const& b) {
                       return BazelCasClient::CreateUpdateBlobsSingleRequest(b);
                   });
    return request;
}

auto BazelCasClient::CreateUpdateBlobsSingleRequest(BazelBlob const& b) noexcept
    -> bazel_re::BatchUpdateBlobsRequest_Request {
    bazel_re::BatchUpdateBlobsRequest_Request r{};
    r.set_allocated_digest(
        gsl::owner<bazel_re::Digest*>{new bazel_re::Digest{b.digest}});
    r.set_data(b.data);
    return r;
}

auto BazelCasClient::CreateGetTreeRequest(
    std::string const& instance_name,
    bazel_re::Digest const& root_digest,
    int page_size,
    std::string const& page_token) noexcept -> bazel_re::GetTreeRequest {
    bazel_re::GetTreeRequest request;
    request.set_instance_name(instance_name);
    request.set_allocated_root_digest(
        gsl::owner<bazel_re::Digest*>{new bazel_re::Digest{root_digest}});
    request.set_page_size(page_size);
    request.set_page_token(page_token);
    return request;
}

template <class T_Content, class T_Inner, class T_Response>
auto BazelCasClient::ProcessBatchResponse(
    T_Response const& response,
    std::function<void(std::vector<T_Content>*, T_Inner const&)> const&
        inserter) const noexcept -> std::vector<T_Content> {
    std::vector<T_Content> output;
    for (auto const& res : response.responses()) {
        auto const& res_status = res.status();
        if (res_status.code() == static_cast<int>(grpc::StatusCode::OK)) {
            inserter(&output, res);
        }
        else {
            LogStatus(&logger_, LogLevel::Debug, res_status);
        }
    }
    return output;
}

template <class T_Content, class T_Response>
auto BazelCasClient::ProcessResponseContents(
    T_Response const& response) const noexcept -> std::vector<T_Content> {
    std::vector<T_Content> output;
    auto const& contents = detail::GetResponseContents<T_Content>(response);
    std::copy(contents.begin(), contents.end(), std::back_inserter(output));
    return output;
}
