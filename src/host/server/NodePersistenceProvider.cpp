#include "host/server/NodePersistenceProvider.hpp"

#include "common/v1/common.pb.h"
#include "datastore/v1/datastore.grpc.pb.h"
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
#include "datastore/development/v1/admission.grpc.pb.h"
#include "gargantuan/assets/AssetTypes.hpp"
#include <atomic>
#endif

#include <google/protobuf/any.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include <grpc/support/time.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace gargantuan::host {
	namespace Wire = ::gargantuan::node::datastore::v1;
	namespace Common = ::gargantuan::node::common::v1;
	namespace Pb = ::google::protobuf;
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
	namespace DevelopmentWire = ::gargantuan::node::datastore::development::v1;
#endif

	namespace {
		constexpr std::size_t MaxTokenBytes = 4096;
		constexpr std::size_t MaxCertificateBytes = 1024 * 1024;
		constexpr std::size_t MaxRequestIdBytes = 64;
		constexpr std::size_t MaxStatusDetailsBytes = 8192;
		constexpr int MaxWireBytes = static_cast<int>(PersistenceMaxPageBytes + 256 * 1024);
		constexpr std::size_t MaxWireQueryBytes = 256 * 1024;

		PersistenceResult WithStatus(PersistenceStatus Status) {
			PersistenceResult Result;
			Result.Status = Status;
			return Result;
		}

		std::optional<std::string> ReadBoundedFile(const std::filesystem::path &Path, std::size_t Maximum) {
			std::error_code Error;
			if (!std::filesystem::is_regular_file(Path, Error) || Error) return std::nullopt;
			const auto Size = std::filesystem::file_size(Path, Error);
			if (Error || Size == 0 || Size > Maximum) return std::nullopt;
			std::ifstream File(Path, std::ios::binary);
			std::string Contents(static_cast<std::size_t>(Size), '\0');
			File.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			if (!File || File.peek() != std::char_traits<char>::eof() || File.bad()) return std::nullopt;
			return Contents;
		}

		std::optional<std::string> ReadWorkloadToken(const std::filesystem::path &Path) {
			auto Token = ReadBoundedFile(Path, MaxTokenBytes + 2);
			if (!Token) return std::nullopt;
			if (Token->ends_with('\n')) {
				Token->pop_back();
				if (Token->ends_with('\r')) Token->pop_back();
			}
			if (Token->empty() || Token->size() > MaxTokenBytes ||
				!std::ranges::all_of(*Token, [](unsigned char Ch) { return Ch >= '!' && Ch <= '~'; })) {
				return std::nullopt;
			}
			return Token;
		}

		bool IsValidEndpoint(std::string_view Endpoint) {
			const auto PortIndex = Endpoint.rfind(':');
			if (Endpoint.size() > 512 || PortIndex == std::string_view::npos || PortIndex == 0 ||
				PortIndex + 1 == Endpoint.size()) return false;
			std::uint16_t Port = 0;
			const auto End = Endpoint.data() + Endpoint.size();
			const auto Parsed = std::from_chars(Endpoint.data() + PortIndex + 1, End, Port);
			if (Parsed.ec != std::errc{} || Parsed.ptr != End || Port == 0) return false;
			return std::ranges::all_of(Endpoint.substr(0, PortIndex), [](unsigned char Ch) {
				return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') ||
					Ch == '.' || Ch == '-' || Ch == ':' || Ch == '[' || Ch == ']';
			});
		}

		bool IsValidRequestId(std::string_view Id) {
			return !Id.empty() && Id.size() <= MaxRequestIdBytes && std::ranges::all_of(Id, [](unsigned char Ch) {
				return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') ||
					Ch == '-' || Ch == '_';
			});
		}

		std::optional<Common::ErrorDetail> ReadErrorDetail(const grpc::Status &Status, const std::string &RequestId) {
			const auto &Bytes = Status.error_details();
			if (Bytes.empty() || Bytes.size() > MaxStatusDetailsBytes) return std::nullopt;
			// grpc-status-details-bin is google.rpc.Status. Read its code and
			// repeated Any details without a generated googleapis dependency.
			Pb::io::CodedInputStream Input(reinterpret_cast<const std::uint8_t *>(Bytes.data()),
				static_cast<int>(Bytes.size()));
			std::optional<Common::ErrorDetail> Result;
			std::uint32_t Code = 0;
			while (const auto Tag = Input.ReadTag()) {
				if (Tag == 8) {
					if (!Input.ReadVarint32(&Code)) return std::nullopt;
				} else if (Tag == 26) {
					int Length = 0;
					std::string Encoded;
					Pb::Any Detail;
					if (!Input.ReadVarintSizeAsInt(&Length) || static_cast<std::size_t>(Length) > Bytes.size() ||
						!Input.ReadString(&Encoded, Length) || !Detail.ParseFromString(Encoded)) return std::nullopt;
					if (Detail.Is<Common::ErrorDetail>()) {
						if (Result || !Detail.UnpackTo(&Result.emplace())) return std::nullopt;
					}
				} else if (!Pb::internal::WireFormatLite::SkipField(&Input, Tag)) {
					return std::nullopt;
				}
			}
			if (!Input.ConsumedEntireMessage() || Code != static_cast<std::uint32_t>(Status.error_code()) ||
				!Result || Result->request_id() != RequestId) return std::nullopt;
			return Result;
		}

		PersistenceStatus MapStatus(const grpc::Status &Status, const PersistenceRequest &Request) {
			const bool Mutation = IsPersistenceMutation(Request.Operation);
			const auto Uncertain = Mutation ? PersistenceStatus::UnknownOutcome : PersistenceStatus::Unavailable;
			switch (Status.error_code()) {
				case grpc::StatusCode::CANCELLED:
					return Mutation ? Uncertain : PersistenceStatus::Cancelled;
				case grpc::StatusCode::DEADLINE_EXCEEDED:
					return Mutation ? Uncertain : PersistenceStatus::Deadline;
				case grpc::StatusCode::NOT_FOUND:
					return PersistenceStatus::Missing;
				case grpc::StatusCode::ALREADY_EXISTS:
					return PersistenceStatus::Conflict;
				case grpc::StatusCode::UNAUTHENTICATED:
				case grpc::StatusCode::PERMISSION_DENIED:
					return PersistenceStatus::Denied;
				case grpc::StatusCode::INVALID_ARGUMENT:
					return PersistenceStatus::Invalid;
				case grpc::StatusCode::UNIMPLEMENTED:
					return PersistenceStatus::Unsupported;
				case grpc::StatusCode::RESOURCE_EXHAUSTED: {
					if (!Mutation) return PersistenceStatus::Busy;
					const auto Detail = ReadErrorDetail(Status, Request.RequestId);
					if (Detail && (Detail->code() == Common::ERROR_CODE_RESOURCE_EXHAUSTED ||
						Detail->code() == Common::ERROR_CODE_RATE_LIMITED)) return PersistenceStatus::Busy;
					return Uncertain;
				}
				case grpc::StatusCode::FAILED_PRECONDITION: {
					if (Request.Operation == PersistenceOperation::Query) {
						const auto Detail = ReadErrorDetail(Status, Request.RequestId);
						if (Detail && Detail->code() == Common::ERROR_CODE_FAILED_PRECONDITION &&
							Detail->message() == "continuation cursor is stale") return PersistenceStatus::StaleCursor;
					}
					return PersistenceStatus::Unavailable;
				}
				case grpc::StatusCode::UNAVAILABLE: {
					if (!Mutation) return PersistenceStatus::Unavailable;
					const auto Detail = ReadErrorDetail(Status, Request.RequestId);
					if (Detail && Detail->code() == Common::ERROR_CODE_UNAVAILABLE && Detail->retryable() &&
						!Detail->metadata().contains("mutation_outcome")) return PersistenceStatus::Unavailable;
					return Uncertain;
				}
				default:
					return Uncertain;
			}
		}

		void EncodeValue(const PersistenceDocumentValue &Value, Pb::Value &WireValue);

		void EncodeObject(const PersistenceDocumentValue::Object &Fields, Pb::Struct &Object) {
			for (const auto &Field : Fields) EncodeValue(Field.Value, (*Object.mutable_fields())[Field.Name]);
		}

		void EncodeValue(const PersistenceDocumentValue &Value, Pb::Value &WireValue) {
			std::visit([&WireValue]<typename T>(const T &Value) {
				if constexpr (std::is_same_v<T, std::monostate>) WireValue.set_null_value(Pb::NULL_VALUE);
				else if constexpr (std::is_same_v<T, bool>) WireValue.set_bool_value(Value);
				else if constexpr (std::is_same_v<T, double>) WireValue.set_number_value(Value);
				else if constexpr (std::is_same_v<T, std::string>) WireValue.set_string_value(Value);
				else if constexpr (std::is_same_v<T, PersistenceDocumentValue::Array>) {
					auto &Array = *WireValue.mutable_list_value();
					for (const auto &Element : Value) EncodeValue(Element, *Array.add_values());
				} else {
					EncodeObject(Value, *WireValue.mutable_struct_value());
				}
			}, Value.Data);
		}

		bool DecodeValue(const Pb::Value &WireValue, PersistenceDocumentValue &Target, std::size_t Depth, std::size_t &Nodes);

		bool DecodeObject(const Pb::Struct &WireObject, PersistenceDocumentValue &Target, std::size_t Depth, std::size_t &Nodes) {
			if (static_cast<std::size_t>(WireObject.fields_size()) > PersistenceMaxDocumentNodes - Nodes) return false;
			auto &Fields = Target.Data.emplace<PersistenceDocumentValue::Object>();
			Fields.reserve(static_cast<std::size_t>(WireObject.fields_size()));
			for (const auto &[Name, WireValue] : WireObject.fields()) {
				PersistenceDocumentValue Value;
				if (!DecodeValue(WireValue, Value, Depth + 1, Nodes)) return false;
				Fields.push_back({Name, std::move(Value)});
			}
			std::ranges::sort(Fields, {}, &PersistenceDocumentField::Name);
			return true;
		}

		bool DecodeValue(const Pb::Value &WireValue, PersistenceDocumentValue &Target, std::size_t Depth, std::size_t &Nodes) {
			if (Depth > PersistenceMaxDocumentDepth || Nodes >= PersistenceMaxDocumentNodes) return false;
			++Nodes;
			switch (WireValue.kind_case()) {
				case Pb::Value::kNullValue: return WireValue.null_value() == Pb::NULL_VALUE;
				case Pb::Value::kBoolValue: Target.Data = WireValue.bool_value(); return true;
				case Pb::Value::kNumberValue:
					if (!std::isfinite(WireValue.number_value())) return false;
					Target.Data = WireValue.number_value();
					return true;
				case Pb::Value::kStringValue: Target.Data = WireValue.string_value(); return true;
				case Pb::Value::kStructValue: return DecodeObject(WireValue.struct_value(), Target, Depth, Nodes);
				case Pb::Value::kListValue: {
					const auto &Values = WireValue.list_value().values();
					if (static_cast<std::size_t>(Values.size()) > PersistenceMaxDocumentNodes - Nodes) return false;
					auto &Array = Target.Data.emplace<PersistenceDocumentValue::Array>();
					Array.reserve(static_cast<std::size_t>(Values.size()));
					for (const auto &Element : Values)
						if (!DecodeValue(Element, Array.emplace_back(), Depth + 1, Nodes)) return false;
					return true;
				}
				case Pb::Value::KIND_NOT_SET: return false;
			}
			return false;
		}

		bool DecodeRecord(const Wire::StoredDocument &Source, PersistenceRecord &Record, std::size_t &DocumentBytes) {
			if (!Source.has_document() || Source.revision() == 0 || !IsPersistenceKey(Source.key())) return false;
			PersistenceDocumentValue Value;
			std::size_t Nodes = 1;
			if (!DecodeObject(Source.document(), Value, 1, Nodes) || !MeasurePersistenceDocument(Value, DocumentBytes) ||
				DocumentBytes > PersistenceMaxValueBytes) return false;
			Record = {Source.key(), std::move(Value), Source.revision()};
			return true;
		}

		Wire::ComparisonOperator GetComparison(PersistenceComparison Operator) {
			switch (Operator) {
				case PersistenceComparison::Equal: return Wire::COMPARISON_OPERATOR_EQ;
				case PersistenceComparison::NotEqual: return Wire::COMPARISON_OPERATOR_NE;
				case PersistenceComparison::Less: return Wire::COMPARISON_OPERATOR_LT;
				case PersistenceComparison::LessOrEqual: return Wire::COMPARISON_OPERATOR_LTE;
				case PersistenceComparison::Greater: return Wire::COMPARISON_OPERATOR_GT;
				case PersistenceComparison::GreaterOrEqual: return Wire::COMPARISON_OPERATOR_GTE;
				case PersistenceComparison::Exists: return Wire::COMPARISON_OPERATOR_EXISTS;
			}
			return Wire::COMPARISON_OPERATOR_UNSPECIFIED;
		}

		template <typename Message>
		void EncodePath(const std::vector<std::string> &Path, Message &Target) {
			for (const auto &Part : Path) Target.add_field_path(Part);
		}

		bool EncodeFilter(const PersistenceFilter &Filter, Wire::QueryFilter &Target) {
			auto &Comparison = *Target.mutable_comparison();
			const auto Operator = GetComparison(Filter.Operator);
			if (Operator == Wire::COMPARISON_OPERATOR_UNSPECIFIED) return false;
			EncodePath(Filter.Path, Comparison);
			Comparison.set_operator_(Operator);
			if (Operator == Wire::COMPARISON_OPERATOR_EXISTS) return true;
			EncodeValue(Filter.Value, *Comparison.mutable_value());
			return true;
		}

		bool EncodeQuery(const PersistenceRequest &Request, Wire::QueryDocumentsRequest &WireRequest) {
			const auto &Query = Request.Query;
			if (!IsPersistenceUtf8(Query.Cursor)) return false;
			WireRequest.set_request_id(Request.RequestId);
			WireRequest.set_collection(Request.Store);
			WireRequest.set_limit(Query.Limit);
			WireRequest.set_continuation_cursor(Query.Cursor);
			if (Query.Filters.size() == 1) {
				if (!EncodeFilter(Query.Filters.front(), *WireRequest.mutable_filter())) return false;
			} else if (!Query.Filters.empty()) {
				auto &Group = *WireRequest.mutable_filter()->mutable_and_();
				for (const auto &Filter : Query.Filters)
					if (!EncodeFilter(Filter, *Group.add_filters())) return false;
			}
			for (const auto &Order : Query.Order) {
				auto &WireOrder = *WireRequest.add_order();
				EncodePath(Order.Path, WireOrder);
				WireOrder.set_direction(Order.Descending ? Wire::ORDER_DIRECTION_DESCENDING : Wire::ORDER_DIRECTION_ASCENDING);
			}
			return WireRequest.ByteSizeLong() <= MaxWireQueryBytes;
		}

		PersistenceResult DecodePage(const PersistenceRequest &Request, const Wire::QueryDocumentsResponse &Response) {
			if (static_cast<std::size_t>(Response.results_size()) > Request.Query.Limit ||
				Response.continuation_cursor().size() > PersistenceMaxCursorBytes ||
				!IsPersistenceUtf8(Response.continuation_cursor()) ||
				(!Response.continuation_cursor().empty() && (Response.results_size() == 0 ||
					Response.continuation_cursor() == Request.Query.Cursor))) return WithStatus(PersistenceStatus::Unavailable);
			std::size_t PageBytes = Response.continuation_cursor().size();
			std::unordered_set<std::string_view> Keys;
			auto Result = WithStatus(PersistenceStatus::Ok);
			Result.Records.reserve(static_cast<std::size_t>(Response.results_size()));
			for (const auto &Document : Response.results()) {
				PersistenceRecord Record;
				std::size_t DocumentBytes = 0;
				if (!Keys.insert(Document.key()).second || !DecodeRecord(Document, Record, DocumentBytes))
					return WithStatus(PersistenceStatus::Unavailable);
				PageBytes += Record.Key.size() + DocumentBytes;
				if (PageBytes > PersistenceMaxPageBytes) return WithStatus(PersistenceStatus::Unavailable);
				Result.Records.push_back(std::move(Record));
			}
			Result.Cursor = Response.continuation_cursor();
			return Result;
		}

		template <typename Message>
		Message MakeRecordRequest(const PersistenceRequest &Request) {
			Message WireRequest;
			WireRequest.set_request_id(Request.RequestId);
			WireRequest.set_key(Request.Key);
			if constexpr (requires { WireRequest.set_store(Request.Store); }) WireRequest.set_store(Request.Store);
			else WireRequest.set_collection(Request.Store);
			if constexpr (requires { WireRequest.set_expected_revision(0); })
				if (Request.ExpectedRevision) WireRequest.set_expected_revision(*Request.ExpectedRevision);
			return WireRequest;
		}

		template <typename Message>
		PersistenceResult DecodeMutation(const PersistenceRequest &Request, const Message &Response) {
			if (Response.revision() <= Request.ExpectedRevision.value_or(0))
				return WithStatus(PersistenceStatus::UnknownOutcome);
			auto Result = WithStatus(PersistenceStatus::Ok);
			Result.Revision = Response.revision();
			return Result;
		}

		class NodeCall final {
		  public:
			NodeCall(const PersistenceRequest &Request, const std::string &Token, std::stop_token Stop,
				const std::string &Grant = {}) : Request(Request), Stop(Stop) {
				Context.AddMetadata("authorization", "Bearer " + Token);
				if (!Grant.empty()) Context.AddMetadata("x-gargantuan-development-grant", Grant);
				// Reads can use their existing deadline to await a reconnect. A
				// mutation never waits through transient channel failure.
				Context.set_wait_for_ready(!IsPersistenceMutation(Request.Operation));
			}

			template <typename Stub, typename WireRequest, typename Response, typename Decode>
			PersistenceResult Invoke(Stub &Client, grpc::Status (Stub::*Rpc)(grpc::ClientContext *, const WireRequest &, Response *),
				const WireRequest &Message, Decode Decoder) {
				if (Stop.stop_requested()) return WithStatus(PersistenceStatus::Cancelled);
				const auto Remaining = Request.Deadline - PersistenceClock::now();
				if (Remaining <= PersistenceClock::duration::zero()) return WithStatus(PersistenceStatus::Deadline);
				const auto Budget = std::min(Remaining, std::chrono::duration_cast<PersistenceClock::duration>(std::chrono::seconds(30)));
				const auto Nanoseconds = std::chrono::ceil<std::chrono::nanoseconds>(Budget);
				Context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(Nanoseconds.count(), GPR_TIMESPAN)));
				const std::stop_callback Cancel(Stop, [this] { Context.TryCancel(); });
				if (Stop.stop_requested()) return WithStatus(PersistenceStatus::Cancelled);
				Response Reply;
				const auto Status = (Client.*Rpc)(&Context, Message, &Reply);
				if (!Status.ok()) {
					if (!IsPersistenceMutation(Request.Operation) && Status.error_code() == grpc::StatusCode::CANCELLED &&
						!Stop.stop_requested() && PersistenceClock::now() >= Request.Deadline)
						return WithStatus(PersistenceStatus::Deadline);
					return WithStatus(MapStatus(Status, Request));
				}
				if (Reply.request_id() != Request.RequestId) return WithStatus(IsPersistenceMutation(Request.Operation)
					? PersistenceStatus::UnknownOutcome : PersistenceStatus::Unavailable);
				return Decoder(Request, Reply);
			}

		  private:
			const PersistenceRequest &Request;
			std::stop_token Stop;
			grpc::ClientContext Context;
		};

		PersistenceResult CallKeyValue(Wire::KeyValueStore::Stub &Client, NodeCall &Call, const PersistenceRequest &Request) {
			using Stub = Wire::KeyValueStore::Stub;
			switch (Request.Operation) {
				case PersistenceOperation::Get:
					return Call.Invoke(Client, &Stub::Get, MakeRecordRequest<Wire::GetKeyValueRequest>(Request),
						[](const PersistenceRequest &Request, const Wire::GetKeyValueResponse &Response) {
							if (Response.revision() == 0 || Response.value().size() > PersistenceMaxValueBytes)
								return WithStatus(PersistenceStatus::Unavailable);
							PersistenceBytes Bytes(Response.value().size());
							if (!Bytes.empty()) std::memcpy(Bytes.data(), Response.value().data(), Bytes.size());
							auto Result = WithStatus(PersistenceStatus::Ok);
							Result.Record = PersistenceRecord{Request.Key, std::move(Bytes), Response.revision()};
							return Result;
						});
				case PersistenceOperation::Put: {
					const auto &Bytes = std::get<PersistenceBytes>(Request.Payload);
					auto Message = MakeRecordRequest<Wire::PutKeyValueRequest>(Request);
					if (!Bytes.empty()) Message.set_value(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
					return Call.Invoke(Client, &Stub::Put, Message, DecodeMutation<Wire::PutKeyValueResponse>);
				}
				case PersistenceOperation::Delete:
					return Call.Invoke(Client, &Stub::Delete, MakeRecordRequest<Wire::DeleteKeyValueRequest>(Request),
						DecodeMutation<Wire::DeleteKeyValueResponse>);
				case PersistenceOperation::Query: return WithStatus(PersistenceStatus::Unsupported);
			}
			return WithStatus(PersistenceStatus::Unsupported);
		}

		PersistenceResult CallDocument(Wire::DocumentStore::Stub &Client, NodeCall &Call, const PersistenceRequest &Request) {
			using Stub = Wire::DocumentStore::Stub;
			switch (Request.Operation) {
				case PersistenceOperation::Get:
					return Call.Invoke(Client, &Stub::Get, MakeRecordRequest<Wire::GetDocumentRequest>(Request),
						[](const PersistenceRequest &Request, const Wire::GetDocumentResponse &Response) {
							PersistenceRecord Record;
							std::size_t DocumentBytes = 0;
							if (!Response.has_result() || Response.result().key() != Request.Key ||
								!DecodeRecord(Response.result(), Record, DocumentBytes)) return WithStatus(PersistenceStatus::Unavailable);
							auto Result = WithStatus(PersistenceStatus::Ok);
							Result.Record = std::move(Record);
							return Result;
						});
				case PersistenceOperation::Put: {
					const auto &Document = std::get<PersistenceDocumentValue>(Request.Payload);
					const auto &Fields = std::get<PersistenceDocumentValue::Object>(Document.Data);
					auto Message = MakeRecordRequest<Wire::PutDocumentRequest>(Request);
					EncodeObject(Fields, *Message.mutable_document());
					return Call.Invoke(Client, &Stub::Put, Message, DecodeMutation<Wire::PutDocumentResponse>);
				}
				case PersistenceOperation::Delete:
					return Call.Invoke(Client, &Stub::Delete, MakeRecordRequest<Wire::DeleteDocumentRequest>(Request),
						DecodeMutation<Wire::DeleteDocumentResponse>);
				case PersistenceOperation::Query: {
					Wire::QueryDocumentsRequest Message;
					if (!EncodeQuery(Request, Message)) return WithStatus(PersistenceStatus::Invalid);
					return Call.Invoke(Client, &Stub::Query, Message, DecodePage);
				}
			}
			return WithStatus(PersistenceStatus::Unsupported);
		}
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		struct NodeDevelopmentLease final {
			std::unique_ptr<DevelopmentWire::DevelopmentAdmission::Stub> Admission;
			std::string Credential;
			std::string Grant;
			std::atomic_bool Retired{false};

			~NodeDevelopmentLease() {
				if (Grant.empty()) return;
				try {
					grpc::ClientContext Context;
					Context.AddMetadata("authorization", "Bearer " + Credential);
					Context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
					DevelopmentWire::ReleaseRequest Request;
					Request.set_grant(Grant);
					DevelopmentWire::ReleaseResponse Reply;
					(void)Admission->Release(&Context, Request, &Reply);
				} catch (const std::exception &) {
					// A stopped peer retires this lease locally. The private server bounds idle grants.
				}
			}
		};

		std::unique_ptr<NodeDevelopmentLease> AdmitDevelopment(const std::shared_ptr<grpc::Channel> &Channel,
			const NodeDevelopmentBinding &Expected, const std::filesystem::path &TokenFile) {
			auto Credential = ReadWorkloadToken(TokenFile);
			if (!Credential) throw std::runtime_error("Private Node workload credential is unavailable");
			auto Lease = std::make_unique<NodeDevelopmentLease>();
			Lease->Admission = DevelopmentWire::DevelopmentAdmission::NewStub(Channel);
			Lease->Credential = std::move(*Credential);
			grpc::ClientContext Context;
			Context.AddMetadata("authorization", "Bearer " + Lease->Credential);
			Context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
			DevelopmentWire::AdmitRequest Request;
			Request.set_version(1);
			Request.set_project_id(Expected.Project.ToString());
			Request.set_nonce(ProjectId::New().ToString());
			DevelopmentWire::AdmitResponse Reply;
			if (!Lease->Admission->Admit(&Context, Request, &Reply).ok() || !ProjectId::Parse(Reply.grant()))
				throw std::runtime_error("Private Node development grant is unavailable");
			Lease->Grant = Reply.grant();
			if (Reply.version() != 1 || Reply.nonce() != Request.nonce() || Reply.project_id() != Request.project_id() ||
				Reply.tenant_id() != Expected.Tenant || Reply.database() != Expected.Database ||
				Reply.database_generation() != Expected.DatabaseGeneration || Reply.node_instance() != Expected.NodeInstance)
				throw std::runtime_error("Private Node development grant did not match the expected binding");
			return Lease;
		}
#endif
	} // namespace

	struct NodePersistenceProvider::Impl final {
		std::filesystem::path WorkloadTokenFile;
		std::unique_ptr<Wire::KeyValueStore::Stub> KeyValues;
		std::unique_ptr<Wire::DocumentStore::Stub> Documents;
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		std::unique_ptr<NodeDevelopmentLease> Development;
#endif
	};

	NodePersistenceProvider::NodePersistenceProvider(NodePersistenceProviderConfiguration Configuration) {
		if (!IsValidEndpoint(Configuration.Endpoint) || Configuration.RootCertificateFile.empty() ||
			Configuration.RootCertificateFile.native().size() > 1024 || Configuration.WorkloadTokenFile.empty() ||
			Configuration.WorkloadTokenFile.native().size() > 1024) {
			throw std::invalid_argument("Invalid Node persistence configuration");
		}
		auto Certificate = ReadBoundedFile(Configuration.RootCertificateFile, MaxCertificateBytes);
		if (!Certificate || Certificate->find('\0') != std::string::npos ||
			Certificate->find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
			throw std::invalid_argument("Node persistence root certificate is unavailable or invalid");
		}
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		if (const auto &Expected = Configuration.Development) {
			if ((!Configuration.Endpoint.starts_with("127.0.0.1:") && !Configuration.Endpoint.starts_with("[::1]:")) ||
				!Expected->Project.IsValid() ||
				AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Certificate->data()),
					Certificate->size())).ToString() != Expected->CertificateSha256)
				throw std::invalid_argument("Private Node TLS identity did not match the expected local binding");
		}
#endif
		grpc::SslCredentialsOptions TLS;
		TLS.pem_root_certs = std::move(*Certificate);
		grpc::ChannelArguments Arguments;
		Arguments.SetMaxReceiveMessageSize(MaxWireBytes);
		Arguments.SetMaxSendMessageSize(MaxWireBytes);
		Arguments.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
		// Reestablish the shared channel promptly after a Node restart. These
		// bounds schedule connection attempts, never a replay of an RPC.
		Arguments.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
		Arguments.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 500);
		Arguments.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 2'000);
		const auto Channel = grpc::CreateCustomChannel(Configuration.Endpoint, grpc::SslCredentials(TLS), Arguments);
		State = std::make_unique<Impl>(Impl{std::move(Configuration.WorkloadTokenFile), Wire::KeyValueStore::NewStub(Channel),
			Wire::DocumentStore::NewStub(Channel)});
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		if (Configuration.Development)
			State->Development = AdmitDevelopment(Channel, *Configuration.Development, State->WorkloadTokenFile);
#endif
	}

	NodePersistenceProvider::~NodePersistenceProvider() = default;

	PersistenceCapabilities NodePersistenceProvider::Capabilities() const noexcept { return {true, true, true}; }

	PersistenceResult NodePersistenceProvider::Execute(const PersistenceRequest &Request, std::stop_token Stop) {
		if (Stop.stop_requested()) return WithStatus(PersistenceStatus::Cancelled);
		if (PersistenceClock::now() >= Request.Deadline) return WithStatus(PersistenceStatus::Deadline);
		if (!IsValidRequestId(Request.RequestId) || !IsPersistenceRequestValid(Request)) {
			return WithStatus(PersistenceStatus::Invalid);
		}
		const auto Token = ReadWorkloadToken(State->WorkloadTokenFile);
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		auto *Development = State->Development.get();
		if (Development && (Development->Retired || !Token || *Token != Development->Credential)) {
			Development->Retired = true;
			return WithStatus(PersistenceStatus::Denied);
		}
#endif
		if (!Token) return WithStatus(PersistenceStatus::Unavailable);
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		NodeCall Call(Request, *Token, Stop, Development ? Development->Grant : std::string{});
#else
		NodeCall Call(Request, *Token, Stop);
#endif
		PersistenceResult Result;
		switch (Request.Kind) {
			case PersistenceStoreKind::KeyValue: Result = CallKeyValue(*State->KeyValues, Call, Request); break;
			case PersistenceStoreKind::Document: Result = CallDocument(*State->Documents, Call, Request); break;
			default: return WithStatus(PersistenceStatus::Unsupported);
		}
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		if (Development && Result.Status == PersistenceStatus::Denied) Development->Retired = true;
#endif
		return Result;
	}
} // namespace gargantuan::host
