#include <ydb/core/keyvalue/keyvalue_events.h>
#include <ydb/core/persqueue/events/internal.h>
#include <ydb/core/persqueue/partition.h>
#include <ydb/core/persqueue/ut/common/pq_ut_common.h>
#include <ydb/core/protos/counters_keyvalue.pb.h>
#include <ydb/core/protos/pqconfig.pb.h>
#include <ydb/core/tablet/tablet_counters_protobuf.h>
#include <ydb/core/tx/tx_processing.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>
#include <ydb/public/api/protos/draft/persqueue_error_codes.pb.h>
#include <ydb/public/lib/base/msgbus_status.h>

#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/actors/core/event.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/json/json_reader.h>

#include <util/generic/hash.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/system/types.h>

#include "make_config.h"
#include "pqtablet_mock.h"

namespace NKikimr::NPQ {

namespace NHelpers {

struct TTxOperation {
    ui32 Partition;
    TMaybe<TString> Consumer;
    TMaybe<ui64> Begin;
    TMaybe<ui64> End;
    TString Path;
    TMaybe<ui32> SupportivePartition;
};

struct TConfigParams {
    TMaybe<NKikimrPQ::TPQTabletConfig> Tablet;
    TMaybe<NKikimrPQ::TBootstrapConfig> Bootstrap;
};

struct TProposeTransactionParams {
    ui64 TxId = 0;
    TVector<ui64> Senders;
    TVector<ui64> Receivers;
    TVector<TTxOperation> TxOps;
    TMaybe<TConfigParams> Configs;
    TMaybe<TWriteId> WriteId;
};

struct TPlanStepParams {
    ui64 Step;
    TVector<ui64> TxIds;
};

struct TReadSetParams {
    ui64 Step = 0;
    ui64 TxId = 0;
    ui64 Source = 0;
    ui64 Target = 0;
    bool Predicate = false;
};

struct TDropTabletParams {
    ui64 TxId = 0;
};

struct TCancelTransactionProposalParams {
    ui64 TxId = 0;
};

struct TGetOwnershipRequestParams {
    TMaybe<ui32> Partition;
    TMaybe<ui64> MsgNo;
    TMaybe<TWriteId> WriteId;
    TMaybe<bool> NeedSupportivePartition;
    TMaybe<TString> Owner; // o
    TMaybe<ui64> Cookie;
};

struct TWriteRequestParams {
    TMaybe<TString> Topic;
    TMaybe<ui32> Partition;
    TMaybe<TString> Owner;
    TMaybe<ui64> MsgNo;
    TMaybe<TWriteId> WriteId;
    TMaybe<TString> SourceId; // w
    TMaybe<ui64> SeqNo;       // w
    TMaybe<TString> Data;     // w
    //TMaybe<TInstant> CreateTime;
    //TMaybe<TInstant> WriteTime;
    TMaybe<ui64> Cookie;
};

struct TAppSendReadSetParams {
  ui64 Step = 0;
  ui64 TxId = 0;
  TMaybe<ui64> SenderId;
  bool Predicate = true;
};

using NKikimr::NPQ::NHelpers::CreatePQTabletMock;
using TPQTabletMock = NKikimr::NPQ::NHelpers::TPQTabletMock;

} // namespace NHelpers

Y_UNIT_TEST_SUITE(TPQTabletTests) {

class TPQTabletFixture : public NUnitTest::TBaseFixture {
protected:

    inline static const TString DEFAULT_OWNER = "-=[ 0wn3r ]=-";
    struct TProposeTransactionResponseMatcher {
        TMaybe<ui64> TxId;
        TMaybe<NKikimrPQ::TEvProposeTransactionResult::EStatus> Status;
    };

    struct TTxOperationMatcher {
        TMaybe<ui32> Partition;
        TMaybe<TString> Consumer;
        TMaybe<ui64> Begin;
        TMaybe<ui64> End;
    };

    struct TCmdWriteTxMatcher {
        TMaybe<ui64> TxId;
        TMaybe<NKikimrPQ::TTransaction::EState> State;
        TVector<ui64> Senders;
        TVector<ui64> Receivers;
        TVector<TTxOperationMatcher> TxOps;
    };

    struct TPlanStepAckMatcher {
        TMaybe<ui64> Step;
        TVector<ui64> TxIds;
    };

    struct TPlanStepAcceptedMatcher {
        TMaybe<ui64> Step;
    };

    struct TReadSetMatcher {
        TMaybe<ui64> Step;
        TMaybe<ui64> TxId;
        TMaybe<ui64> Source;
        TMaybe<ui64> Target;
        TMaybe<NKikimrTx::TReadSetData::EDecision> Decision;
        TMaybe<ui64> Producer;
        TMaybe<size_t> Count;
    };

    struct TReadSetAckMatcher {
        TMaybe<ui64> Step;
        TMaybe<ui64> TxId;
        TMaybe<ui64> Source;
        TMaybe<ui64> Target;
        TMaybe<ui64> Consumer;
    };

    struct TDropTabletReplyMatcher {
        TMaybe<NKikimrProto::EReplyStatus> Status;
        TMaybe<ui64> TxId;
        TMaybe<ui64> TabletId;
        TMaybe<NKikimrPQ::ETabletState> State;
    };

    struct TGetOwnershipResponseMatcher {
        TMaybe<ui64> Cookie;
        TMaybe<NMsgBusProxy::EResponseStatus> Status;
        TMaybe<NPersQueue::NErrorCode::EErrorCode> ErrorCode;
    };

    struct TWriteResponseMatcher {
        TMaybe<ui64> Cookie;
    };

    struct TAppSendReadSetMatcher {
        TMaybe<bool> Status;
    };

    struct TSendReadSetViaAppTestParams {
        size_t TabletsCount = 0;
        NKikimrTx::TReadSetData::EDecision Decision = NKikimrTx::TReadSetData::DECISION_UNKNOWN;
        size_t TabletsRSCount = 0;
        NKikimrTx::TReadSetData::EDecision AppDecision = NKikimrTx::TReadSetData::DECISION_UNKNOWN;
        bool ExpectedAppResponseStatus = true;
        NKikimrPQ::TEvProposeTransactionResult::EStatus ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::COMPLETE;
    };


    using TProposeTransactionParams = NHelpers::TProposeTransactionParams;
    using TPlanStepParams = NHelpers::TPlanStepParams;
    using TReadSetParams = NHelpers::TReadSetParams;
    using TDropTabletParams = NHelpers::TDropTabletParams;
    using TCancelTransactionProposalParams = NHelpers::TCancelTransactionProposalParams;
    using TGetOwnershipRequestParams = NHelpers::TGetOwnershipRequestParams;
    using TWriteRequestParams = NHelpers::TWriteRequestParams;
    using TAppSendReadSetParams = NHelpers::TAppSendReadSetParams;

    void SetUp(NUnitTest::TTestContext&) override;
    void TearDown(NUnitTest::TTestContext&) override;

    void ResetPipe();
    void EnsurePipeExist();
    void SendToPipe(const TActorId& sender,
                    IEventBase* event,
                    ui32 node = 0, ui64 cookie = 0);

    void SendProposeTransactionRequest(const TProposeTransactionParams& params);
    void WaitProposeTransactionResponse(const TProposeTransactionResponseMatcher& matcher = {});

    void SendPlanStep(const TPlanStepParams& params);
    void WaitPlanStepAck(const TPlanStepAckMatcher& matcher = {});
    void WaitPlanStepAccepted(const TPlanStepAcceptedMatcher& matcher = {});

    void WaitReadSet(NHelpers::TPQTabletMock& tablet, const TReadSetMatcher& matcher);
    void WaitReadSetEx(NHelpers::TPQTabletMock& tablet, const TReadSetMatcher& matcher);
    void SendReadSet(const TReadSetParams& params);

    void WaitReadSetAck(NHelpers::TPQTabletMock& tablet, const TReadSetAckMatcher& matcher);
    void SendReadSetAck(NHelpers::TPQTabletMock& tablet);
    void WaitForNoReadSetAck(NHelpers::TPQTabletMock& tablet);

    void SendDropTablet(const TDropTabletParams& params);
    void WaitDropTabletReply(const TDropTabletReplyMatcher& matcher);

    void StartPQWriteStateObserver();
    void WaitForPQWriteState();

    void SendCancelTransactionProposal(const TCancelTransactionProposalParams& params);

    void StartPQWriteTxsObserver(TAutoPtr<IEventHandle>* ev = nullptr);
    void WaitForPQWriteTxs();

    template <class T> void WaitForEvent(size_t count);
    void WaitForCalcPredicateResult(size_t count = 1);
    void WaitForProposePartitionConfigResult(size_t count = 1);

    void TestWaitingForTEvReadSet(size_t senders, size_t receivers);

    void StartPQWriteObserver(bool& flag, unsigned cookie, TAutoPtr<IEventHandle>* ev = nullptr);
    void WaitForPQWriteComplete(bool& flag);

    bool FoundPQWriteState = false;
    bool FoundPQWriteTxs = false;

    void SendGetOwnershipRequest(const TGetOwnershipRequestParams& params);
    // returns ownerCookie
    TString WaitGetOwnershipResponse(const TGetOwnershipResponseMatcher& matcher);
    void SyncGetOwnership(const TGetOwnershipRequestParams& params,
                             const TGetOwnershipResponseMatcher& matcher);

    void SendWriteRequest(const TWriteRequestParams& params);
    void WaitWriteResponse(const TWriteResponseMatcher& matcher);

    // returns owner cookie for this supportive partition
    TString CreateSupportivePartitionForKafka(const NKafka::TProducerInstanceId& producerInstanceId);
    void SendKafkaTxnWriteRequest(const NKafka::TProducerInstanceId& producerInstanceId, const TString& ownerCookie);

    std::unique_ptr<TEvPersQueue::TEvRequest> MakeGetOwnershipRequest(const TGetOwnershipRequestParams& params,
                                                                      const TActorId& pipe) const;

    void TestMultiplePQTablets(const TString& consumer1, const TString& consumer2);
    void TestParallelTransactions(const TString& consumer1, const TString& consumer2);

    void StartPQCalcPredicateObserver(size_t& received);
    void WaitForPQCalcPredicate(size_t& received, size_t expected);

    void WaitForTxState(ui64 txId, NKikimrPQ::TTransaction::EState state);
    void WaitForExecStep(ui64 step);

    void InterceptSaveTxState(TAutoPtr<IEventHandle>& event);
    void SendSaveTxState(TAutoPtr<IEventHandle>& event);

    void WaitForTheTransactionToBeDeleted(ui64 txId);

    TVector<TString> WaitForExactSupportivePartitionsCount(ui32 expectedCount);
    TVector<TString> GetSupportivePartitionsKeysFromKV();
    NKikimrPQ::TTabletTxInfo WaitForExactTxWritesCount(ui32 expectedCount);
    NKikimrPQ::TTabletTxInfo GetTxWritesFromKV();

    void SendAppSendRsRequest(const TAppSendReadSetParams& params);
    void WaitForAppSendRsResponse(const TAppSendReadSetMatcher& matcher);
    void TestSendingTEvReadSetViaApp(const TSendReadSetViaAppTestParams& params);

    //
    // TODO(abcdef): для тестирования повторных вызовов нужны примитивы Send+Wait
    //

    NHelpers::TPQTabletMock* CreatePQTabletMock(ui64 tabletId);

    TMaybe<TTestContext> Ctx;
    TMaybe<TFinalizer> Finalizer;

    TTestActorRuntimeBase::TEventObserver PrevEventObserver;

    TActorId Pipe;
};

void TPQTabletFixture::SetUp(NUnitTest::TTestContext&)
{
    Ctx.ConstructInPlace();
    Ctx->EnableDetailedPQLog = true;

    Finalizer.ConstructInPlace(*Ctx);

    Ctx->Prepare();
    Ctx->Runtime->SetScheduledLimit(5'000);
}

void TPQTabletFixture::TearDown(NUnitTest::TTestContext&)
{
    ResetPipe();
}

void TPQTabletFixture::ResetPipe()
{
    if (Pipe != TActorId()) {
        Ctx->Runtime->ClosePipe(Pipe, Ctx->Edge, 0);
        Pipe = TActorId();
    }
}

void TPQTabletFixture::EnsurePipeExist()
{
    if (Pipe == TActorId()) {
        Pipe = Ctx->Runtime->ConnectToPipe(Ctx->TabletId,
                                           Ctx->Edge,
                                           0,
                                           GetPipeConfigWithRetries());
    }

    Y_ABORT_UNLESS(Pipe != TActorId());
}

void TPQTabletFixture::SendToPipe(const TActorId& sender,
                                  IEventBase* event,
                                  ui32 node, ui64 cookie)
{
    EnsurePipeExist();

    Ctx->Runtime->SendToPipe(Pipe,
                             sender,
                             event,
                             node, cookie);
}

void TPQTabletFixture::SendProposeTransactionRequest(const TProposeTransactionParams& params)
{
    auto event = MakeHolder<TEvPersQueue::TEvProposeTransactionBuilder>();
    THashSet<ui32> partitions;

    ActorIdToProto(Ctx->Edge, event->Record.MutableSourceActor());
    event->Record.SetTxId(params.TxId);

    if (params.Configs) {
        //
        // TxBody.Config
        //
        auto* body = event->Record.MutableConfig();
        if (params.Configs->Tablet.Defined()) {
            *body->MutableTabletConfig() = *params.Configs->Tablet;
        }
        if (params.Configs->Bootstrap.Defined()) {
            *body->MutableBootstrapConfig() = *params.Configs->Bootstrap;
        }
    } else {
        //
        // TxBody.Data
        //
        auto* body = event->Record.MutableData();
        for (auto& txOp : params.TxOps) {
            auto* operation = body->MutableOperations()->Add();
            operation->SetPartitionId(txOp.Partition);
            if (txOp.Begin.Defined()) {
                operation->SetCommitOffsetsBegin(*txOp.Begin);
                operation->SetCommitOffsetsEnd(*txOp.End);
                operation->SetConsumer(*txOp.Consumer);
            }
            operation->SetPath(txOp.Path);
            if (txOp.SupportivePartition.Defined()) {
                operation->SetSupportivePartition(*txOp.SupportivePartition);
            }

            partitions.insert(txOp.Partition);
        }
        for (ui64 tabletId : params.Senders) {
            body->AddSendingShards(tabletId);
        }
        for (ui64 tabletId : params.Receivers) {
            body->AddReceivingShards(tabletId);
        }
        if (params.WriteId) {
            SetWriteId(*body, *params.WriteId);
        }
        body->SetImmediate(params.Senders.empty() && params.Receivers.empty() && (partitions.size() == 1) && !params.WriteId.Defined());
    }

    SendToPipe(Ctx->Edge,
               event.Release());
}

void TPQTabletFixture::WaitProposeTransactionResponse(const TProposeTransactionResponseMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvProposeTransactionResult>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.TxId) {
        UNIT_ASSERT(event->Record.HasTxId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, event->Record.GetTxId());
    }

    if (matcher.Status) {
        UNIT_ASSERT(event->Record.HasStatus());
        UNIT_ASSERT_EQUAL_C(*matcher.Status, event->Record.GetStatus(),
                            "expected: " << NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(*matcher.Status) <<
                            ", received " << NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(event->Record.GetStatus()));
    }
}

void TPQTabletFixture::SendPlanStep(const TPlanStepParams& params)
{
    auto event = MakeHolder<TEvTxProcessing::TEvPlanStep>();
    event->Record.SetStep(params.Step);
    for (ui64 txId : params.TxIds) {
        auto tx = event->Record.AddTransactions();

        tx->SetTxId(txId);
        ActorIdToProto(Ctx->Edge, tx->MutableAckTo());
    }

    SendToPipe(Ctx->Edge,
               event.Release());
}

void TPQTabletFixture::WaitPlanStepAck(const TPlanStepAckMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvTxProcessing::TEvPlanStepAck>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Step.Defined()) {
        UNIT_ASSERT(event->Record.HasStep());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, event->Record.GetStep());
    }

    UNIT_ASSERT_VALUES_EQUAL(matcher.TxIds.size(), event->Record.TxIdSize());
    for (size_t i = 0; i < event->Record.TxIdSize(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(matcher.TxIds[i], event->Record.GetTxId(i));
    }
}

void TPQTabletFixture::WaitPlanStepAccepted(const TPlanStepAcceptedMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvTxProcessing::TEvPlanStepAccepted>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Step.Defined()) {
        UNIT_ASSERT(event->Record.HasStep());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, event->Record.GetStep());
    }
}

void TPQTabletFixture::WaitReadSet(NHelpers::TPQTabletMock& tablet, const TReadSetMatcher& matcher)
{
    if (!tablet.ReadSet.Defined()) {
        TDispatchOptions options;
        options.CustomFinalCondition = [&]() {
            return tablet.ReadSet.Defined();
        };
        UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
    }

    auto readSet = std::move(*tablet.ReadSet);
    tablet.ReadSet = Nothing();

    if (matcher.Step.Defined()) {
        UNIT_ASSERT(readSet.HasStep());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, readSet.GetStep());
    }
    if (matcher.TxId.Defined()) {
        UNIT_ASSERT(readSet.HasTxId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, readSet.GetTxId());
    }
    if (matcher.Source.Defined()) {
        UNIT_ASSERT(readSet.HasTabletSource());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Source, readSet.GetTabletSource());
    }
    if (matcher.Target.Defined()) {
        UNIT_ASSERT(readSet.HasTabletDest());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Target, readSet.GetTabletDest());
    }
    if (matcher.Decision.Defined()) {
        UNIT_ASSERT(readSet.HasReadSet());

        NKikimrTx::TReadSetData data;
        Y_ABORT_UNLESS(data.ParseFromString(readSet.GetReadSet()));

        UNIT_ASSERT_EQUAL(*matcher.Decision, data.GetDecision());
    }
    if (matcher.Producer.Defined()) {
        UNIT_ASSERT(readSet.HasTabletProducer());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Producer, readSet.GetTabletProducer());
    }
}

void TPQTabletFixture::WaitReadSetEx(NHelpers::TPQTabletMock& tablet, const TReadSetMatcher& matcher)
{
    TDispatchOptions options;
    options.CustomFinalCondition = [&]() {
        return tablet.ReadSets[std::make_pair(*matcher.Step, *matcher.TxId)].size() >= *matcher.Count;
    };
    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
}

void TPQTabletFixture::SendReadSet(const TReadSetParams& params)
{
    NKikimrTx::TReadSetData payload;
    payload.SetDecision(params.Predicate ? NKikimrTx::TReadSetData::DECISION_COMMIT : NKikimrTx::TReadSetData::DECISION_ABORT);

    TString body;
    Y_ABORT_UNLESS(payload.SerializeToString(&body));

    auto event = std::make_unique<TEvTxProcessing::TEvReadSet>(params.Step,
                                                               params.TxId,
                                                               params.Source,
                                                               params.Target,
                                                               params.Source,
                                                               body,
                                                               0);

    SendToPipe(Ctx->Edge,
               event.release());
}

void TPQTabletFixture::WaitReadSetAck(NHelpers::TPQTabletMock& tablet, const TReadSetAckMatcher& matcher)
{
    if (!tablet.ReadSetAck.Defined()) {
        TDispatchOptions options;
        options.CustomFinalCondition = [&]() {
            return tablet.ReadSetAck.Defined();
        };
        UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
    }

    if (matcher.Step.Defined()) {
        UNIT_ASSERT(tablet.ReadSetAck->HasStep());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Step, tablet.ReadSetAck->GetStep());
    }
    if (matcher.TxId.Defined()) {
        UNIT_ASSERT(tablet.ReadSetAck->HasTxId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, tablet.ReadSetAck->GetTxId());
    }
    if (matcher.Source.Defined()) {
        UNIT_ASSERT(tablet.ReadSetAck->HasTabletSource());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Source, tablet.ReadSetAck->GetTabletSource());
    }
    if (matcher.Target.Defined()) {
        UNIT_ASSERT(tablet.ReadSetAck->HasTabletDest());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Target, tablet.ReadSetAck->GetTabletDest());
    }
    if (matcher.Consumer.Defined()) {
        UNIT_ASSERT(tablet.ReadSetAck->HasTabletConsumer());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Consumer, tablet.ReadSetAck->GetTabletConsumer());
    }
}

void TPQTabletFixture::WaitForNoReadSetAck(NHelpers::TPQTabletMock& tablet)
{
    TDispatchOptions options;
    options.CustomFinalCondition = [&]() {
        return tablet.ReadSetAck.Defined();
    };
    Ctx->Runtime->DispatchEvents(options, TDuration::Seconds(2));

    UNIT_ASSERT(!tablet.ReadSetAck.Defined());
}

void TPQTabletFixture::SendDropTablet(const TDropTabletParams& params)
{
    auto event = MakeHolder<TEvPersQueue::TEvDropTablet>();
    event->Record.SetTxId(params.TxId);
    event->Record.SetRequestedState(NKikimrPQ::EDropped);

    SendToPipe(Ctx->Edge,
               event.Release());
}

void TPQTabletFixture::WaitDropTabletReply(const TDropTabletReplyMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvDropTabletReply>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Status.Defined()) {
        UNIT_ASSERT(event->Record.HasStatus());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Status, event->Record.GetStatus());
    }
    if (matcher.TxId.Defined()) {
        UNIT_ASSERT(event->Record.HasTxId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TxId, event->Record.GetTxId());
    }
    if (matcher.TabletId.Defined()) {
        UNIT_ASSERT(event->Record.HasTabletId());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.TabletId, event->Record.GetTabletId());
    }
    if (matcher.State.Defined()) {
        UNIT_ASSERT(event->Record.HasActualState());
        UNIT_ASSERT_EQUAL(*matcher.State, event->Record.GetActualState());
    }
}

template <class T>
void TPQTabletFixture::WaitForEvent(size_t count)
{
    bool found = false;
    size_t received = 0;

    TTestActorRuntimeBase::TEventObserver prev;
    auto observer = [&found, &prev, &received, count](TAutoPtr<IEventHandle>& event) {
        if (auto* msg = event->CastAsLocal<T>()) {
            ++received;
            found = (received >= count);
        }

        return prev ? prev(event) : TTestActorRuntimeBase::EEventAction::PROCESS;
    };

    prev = Ctx->Runtime->SetObserverFunc(observer);

    TDispatchOptions options;
    options.CustomFinalCondition = [&found]() {
        return found;
    };

    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));

    Ctx->Runtime->SetObserverFunc(prev);
}

void TPQTabletFixture::WaitForCalcPredicateResult(size_t count)
{
    WaitForEvent<TEvPQ::TEvTxCalcPredicateResult>(count);
}

void TPQTabletFixture::WaitForProposePartitionConfigResult(size_t count)
{
    WaitForEvent<TEvPQ::TEvProposePartitionConfigResult>(count);
}

std::unique_ptr<TEvPersQueue::TEvRequest> TPQTabletFixture::MakeGetOwnershipRequest(const TGetOwnershipRequestParams& params,
                                                                                    const TActorId& pipe) const
{
    auto event = std::make_unique<TEvPersQueue::TEvRequest>();
    auto* request = event->Record.MutablePartitionRequest();
    auto* command = request->MutableCmdGetOwnership();

    if (params.Partition.Defined()) {
        request->SetPartition(*params.Partition);
    }
    if (params.MsgNo.Defined()) {
        request->SetMessageNo(*params.MsgNo);
    }
    if (params.WriteId.Defined()) {
        SetWriteId(*request, *params.WriteId);
    }
    if (params.NeedSupportivePartition.Defined()) {
        request->SetNeedSupportivePartition(*params.NeedSupportivePartition);
    }
    if (params.Cookie.Defined()) {
        request->SetCookie(*params.Cookie);
    }

    ActorIdToProto(pipe, request->MutablePipeClient());

    if (params.Owner.Defined()) {
        command->SetOwner(*params.Owner);
    }

    command->SetForce(true);

    return event;
}

void TPQTabletFixture::SyncGetOwnership(const TGetOwnershipRequestParams& params,
                                        const TGetOwnershipResponseMatcher& matcher)
{
    TActorId pipe = Ctx->Runtime->ConnectToPipe(Ctx->TabletId,
                                                Ctx->Edge,
                                                0,
                                                GetPipeConfigWithRetries());

    auto request = MakeGetOwnershipRequest(params, pipe);
    Ctx->Runtime->SendToPipe(pipe,
                             Ctx->Edge,
                             request.release(),
                             0, 0);
    WaitGetOwnershipResponse(matcher);

    Ctx->Runtime->ClosePipe(pipe, Ctx->Edge, 0);
}

void TPQTabletFixture::SendGetOwnershipRequest(const TGetOwnershipRequestParams& params)
{
    EnsurePipeExist();

    auto request = MakeGetOwnershipRequest(params, Pipe);

    SendToPipe(Ctx->Edge,
               request.release());
}

// returns owner cookie
TString TPQTabletFixture::WaitGetOwnershipResponse(const TGetOwnershipResponseMatcher& matcher)
{
    auto event = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>();
    UNIT_ASSERT(event != nullptr);

    if (matcher.Cookie.Defined()) {
        UNIT_ASSERT(event->Record.GetPartitionResponse().HasCookie());
        UNIT_ASSERT_VALUES_EQUAL(*matcher.Cookie, event->Record.GetPartitionResponse().GetCookie());
    }
    if (matcher.Status.Defined()) {
        UNIT_ASSERT(event->Record.HasStatus());
        UNIT_ASSERT_VALUES_EQUAL((int)*matcher.Status, (int)event->Record.GetStatus());
    }
    if (matcher.ErrorCode.Defined()) {
        UNIT_ASSERT(event->Record.HasErrorCode());
        UNIT_ASSERT_VALUES_EQUAL((int)*matcher.ErrorCode, (int)event->Record.GetErrorCode());
    }

    return event->Record.GetPartitionResponse().GetCmdGetOwnershipResult().GetOwnerCookie();
}

void TPQTabletFixture::SendWriteRequest(const TWriteRequestParams& params)
{
    auto event = MakeHolder<TEvPersQueue::TEvRequest>();
    auto* request = event->Record.MutablePartitionRequest();

    if (params.Topic.Defined()) {
        request->SetTopic(*params.Topic);
    }
    if (params.Partition.Defined()) {
        request->SetPartition(*params.Partition);
    }
    if (params.Owner.Defined()) {
        request->SetOwnerCookie(*params.Owner);
    }
    if (params.MsgNo.Defined()) {
        request->SetMessageNo(*params.MsgNo);
    }
    if (params.WriteId.Defined()) {
        SetWriteId(*request, *params.WriteId);
    }
    if (params.Cookie.Defined()) {
        request->SetCookie(*params.Cookie);
    }

    EnsurePipeExist();
    ActorIdToProto(Pipe, request->MutablePipeClient());

    auto* command = request->AddCmdWrite();

    if (params.SourceId.Defined()) {
        command->SetSourceId(*params.SourceId);
    }
    if (params.SeqNo.Defined()) {
        command->SetSeqNo(*params.SeqNo);
    }
    if (params.Data.Defined()) {
        command->SetData(*params.Data);
    }

    SendToPipe(Ctx->Edge,
               event.Release());
}

TString TPQTabletFixture::CreateSupportivePartitionForKafka(const NKafka::TProducerInstanceId& producerInstanceId) {
    EnsurePipeExist();

    auto request = MakeGetOwnershipRequest({.Partition=0,
                     .WriteId=TWriteId{producerInstanceId},
                     .NeedSupportivePartition=true,
                     .Owner=DEFAULT_OWNER,
                     .Cookie=4}, Pipe);
    Ctx->Runtime->SendToPipe(Pipe,
                             Ctx->Edge,
                             request.release(),
                             0, 0);

    return WaitGetOwnershipResponse({.Cookie=4, .Status=NMsgBusProxy::MSTATUS_OK});
}

void TPQTabletFixture::SendKafkaTxnWriteRequest(const NKafka::TProducerInstanceId& producerInstanceId, const TString& ownerCookie) {
    auto event = MakeHolder<TEvPersQueue::TEvRequest>();
    auto* request = event->Record.MutablePartitionRequest();
    request->SetTopic("/topic");
    request->SetPartition(0);
    request->SetCookie(123);
    request->SetOwnerCookie(ownerCookie);
    request->SetMessageNo(0);

    auto* writeId = request->MutableWriteId();
    writeId->SetKafkaTransaction(true);
    auto* requestProducerInstanceId = writeId->MutableKafkaProducerInstanceId();
    requestProducerInstanceId->SetId(producerInstanceId.Id);
    requestProducerInstanceId->SetEpoch(producerInstanceId.Epoch);

    EnsurePipeExist();
    ActorIdToProto(Pipe, request->MutablePipeClient());

    auto cmdWrite = request->AddCmdWrite();
    cmdWrite->SetSourceId(std::to_string(producerInstanceId.Id));
    cmdWrite->SetSeqNo(0);
    TString data = "123test123";
    cmdWrite->SetData(data);
    cmdWrite->SetCreateTimeMS(TInstant::Now().MilliSeconds());
    cmdWrite->SetDisableDeduplication(true);
    cmdWrite->SetUncompressedSize(data.size());
    cmdWrite->SetIgnoreQuotaDeadline(true);
    cmdWrite->SetExternalOperation(true);

    SendToPipe(Ctx->Edge, event.Release());

    // wait for response
    auto response = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>();
    UNIT_ASSERT(response != nullptr);
    UNIT_ASSERT(response->Record.GetPartitionResponse().HasCookie());
    UNIT_ASSERT_VALUES_EQUAL(123, response->Record.GetPartitionResponse().GetCookie());
}

void TPQTabletFixture::WaitWriteResponse(const TWriteResponseMatcher& matcher)
{
    bool found = false;

    auto observer = [&found, &matcher](TAutoPtr<IEventHandle>& event) {
        if (auto* msg = event->CastAsLocal<TEvPersQueue::TEvResponse>()) {
            if (matcher.Cookie.Defined()) {
                if (msg->Record.HasCookie() && (*matcher.Cookie == msg->Record.GetCookie())) {
                    found = true;
                }
            }
        }

        return TTestActorRuntimeBase::EEventAction::PROCESS;
    };

    auto prev = Ctx->Runtime->SetObserverFunc(observer);

    TDispatchOptions options;
    options.CustomFinalCondition = [&found]() {
        return found;
    };

    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));

    Ctx->Runtime->SetObserverFunc(prev);
}

void TPQTabletFixture::StartPQWriteObserver(bool& flag, unsigned cookie, TAutoPtr<IEventHandle>* ev)
{
    flag = false;

    auto observer = [&flag, cookie, ev](TAutoPtr<IEventHandle>& event) {
        if (auto* kvResponse = event->CastAsLocal<TEvKeyValue::TEvResponse>()) {
            if ((event->Sender == event->Recipient) &&
                kvResponse->Record.HasCookie() &&
                (kvResponse->Record.GetCookie() == cookie)) {
                flag = true;

                if (ev) {
                    *ev = event;
                    return TTestActorRuntimeBase::EEventAction::DROP;
                }
            }
        }

        return TTestActorRuntimeBase::EEventAction::PROCESS;
    };

    Ctx->Runtime->SetObserverFunc(observer);
}

void TPQTabletFixture::WaitForPQWriteComplete(bool& flag)
{
    TDispatchOptions options;
    options.CustomFinalCondition = [&flag]() {
        return flag;
    };
    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
}

void TPQTabletFixture::StartPQWriteStateObserver()
{
    StartPQWriteObserver(FoundPQWriteState, 4); // TPersQueue::WRITE_STATE_COOKIE
}

void TPQTabletFixture::WaitForPQWriteState()
{
    WaitForPQWriteComplete(FoundPQWriteState);
}

void TPQTabletFixture::SendCancelTransactionProposal(const TCancelTransactionProposalParams& params)
{
    auto event = MakeHolder<TEvPersQueue::TEvCancelTransactionProposal>(params.TxId);

    SendToPipe(Ctx->Edge,
               event.Release());
}

void TPQTabletFixture::StartPQWriteTxsObserver(TAutoPtr<IEventHandle>* event)
{
    StartPQWriteObserver(FoundPQWriteTxs, 5, event); // TPersQueue::WRITE_TX_COOKIE
}

void TPQTabletFixture::WaitForPQWriteTxs()
{
    WaitForPQWriteComplete(FoundPQWriteTxs);
}

NHelpers::TPQTabletMock* TPQTabletFixture::CreatePQTabletMock(ui64 tabletId)
{
    NHelpers::TPQTabletMock* mock = nullptr;
    auto wrapCreatePQTabletMock = [&](const NActors::TActorId& tablet, NKikimr::TTabletStorageInfo* info) -> IActor* {
        mock = NHelpers::CreatePQTabletMock(tablet, info);
        return mock;
    };

    CreateTestBootstrapper(*Ctx->Runtime,
                           CreateTestTabletInfo(tabletId, NKikimrTabletBase::TTabletTypes::Dummy, TErasureType::ErasureNone),
                           wrapCreatePQTabletMock);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    Ctx->Runtime->DispatchEvents(options);

    return mock;
}

void TPQTabletFixture::TestMultiplePQTablets(const TString& consumer1, const TString& consumer2)
{
    TVector<std::pair<TString, bool>> consumers;
    consumers.emplace_back(consumer1, true);
    if (consumer1 != consumer2) {
        consumers.emplace_back(consumer2, true);
    }

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, consumers, *Ctx);

    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;

    SendProposeTransactionRequest({.TxId=txId_1,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer=consumer1, .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendProposeTransactionRequest({.TxId=txId_2,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer=consumer2, .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId_2}});
    SendPlanStep({.Step=200, .TxIds={txId_1}});

    WaitPlanStepAck({.Step=100, .TxIds={txId_2}}); // TEvPlanStepAck for Coordinator
    WaitPlanStepAccepted({.Step=100});

    WaitPlanStepAck({.Step=200, .TxIds={txId_1}}); // TEvPlanStepAck for Coordinator
    WaitPlanStepAccepted({.Step=200});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId_2, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId_2, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitReadSet(*tablet, {.Step=200, .TxId=txId_1, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=200, .TxId=txId_1, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

Y_UNIT_TEST_F(Multiple_PQTablets_1, TPQTabletFixture)
{
    TestMultiplePQTablets("consumer", "consumer");
}

Y_UNIT_TEST_F(Multiple_PQTablets_2, TPQTabletFixture)
{
    TestMultiplePQTablets("consumer-1", "consumer-2");
}

void TPQTabletFixture::TestParallelTransactions(const TString& consumer1, const TString& consumer2)
{
    TVector<std::pair<TString, bool>> consumers;
    consumers.emplace_back(consumer1, true);
    if (consumer1 != consumer2) {
        consumers.emplace_back(consumer2, true);
    }

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, consumers, *Ctx);

    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;

    SendProposeTransactionRequest({.TxId=txId_1,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer=consumer1, .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendProposeTransactionRequest({.TxId=txId_2,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer=consumer2, .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    size_t calcPredicateResultCount = 0;
    StartPQCalcPredicateObserver(calcPredicateResultCount);

    // Transactions are planned in reverse order
    SendPlanStep({.Step=100, .TxIds={txId_2}});
    SendPlanStep({.Step=200, .TxIds={txId_1}});

    WaitPlanStepAck({.Step=100, .TxIds={txId_2}}); // TEvPlanStepAck for Coordinator
    WaitPlanStepAccepted({.Step=100});

    WaitPlanStepAck({.Step=200, .TxIds={txId_1}}); // TEvPlanStepAck for Coordinator
    WaitPlanStepAccepted({.Step=200});

    // The PQ tablet sends to the TEvTxCalcPredicate partition for both transactions
    WaitForPQCalcPredicate(calcPredicateResultCount, 2);

    // TEvReadSet messages arrive in any order
    tablet->SendReadSet(*Ctx->Runtime, {.Step=200, .TxId=txId_1, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId_2, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    // Transactions will be executed in the order they were planned
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

void TPQTabletFixture::StartPQCalcPredicateObserver(size_t& received)
{
    received = 0;

    auto observer = [&received](TAutoPtr<IEventHandle>& event) {
        if (auto* msg = event->CastAsLocal<TEvPQ::TEvTxCalcPredicate>()) {
            ++received;
        }

        return TTestActorRuntimeBase::EEventAction::PROCESS;
    };

    Ctx->Runtime->SetObserverFunc(observer);
}

void TPQTabletFixture::WaitForPQCalcPredicate(size_t& received, size_t expected)
{
    TDispatchOptions options;
    options.CustomFinalCondition = [&received, expected]() {
        return received >= expected;
    };
    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
}

void TPQTabletFixture::WaitForTxState(ui64 txId, NKikimrPQ::TTransaction::EState state)
{
    const TString key = GetTxKey(txId);

    while (true) {
        auto request = std::make_unique<TEvKeyValue::TEvRequest>();
        request->Record.SetCookie(12345);
        auto cmd = request->Record.AddCmdReadRange();
        auto range = cmd->MutableRange();
        range->SetFrom(key);
        range->SetIncludeFrom(true);
        range->SetTo(key);
        range->SetIncludeTo(true);
        cmd->SetIncludeData(true);
        SendToPipe(Ctx->Edge, request.release());

        auto response = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>();
        UNIT_ASSERT_VALUES_EQUAL(response->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK);
        const auto& result = response->Record.GetReadRangeResult(0);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), static_cast<ui32>(NKikimrProto::OK));
        const auto& pair = result.GetPair(0);

        NKikimrPQ::TTransaction tx;
        Y_ABORT_UNLESS(tx.ParseFromString(pair.GetValue()));

        if (tx.GetState() == state) {
            return;
        }
    }

    UNIT_FAIL("transaction " << txId << " has not entered the " << state << " state");
}

void TPQTabletFixture::WaitForExecStep(ui64 step)
{
    while (true) {
        auto request = std::make_unique<TEvKeyValue::TEvRequest>();
        request->Record.SetCookie(12345);
        auto cmd = request->Record.AddCmdReadRange();
        auto range = cmd->MutableRange();
        range->SetFrom("_txinfo");
        range->SetIncludeFrom(true);
        range->SetTo("_txinfo");
        range->SetIncludeTo(true);
        cmd->SetIncludeData(true);
        SendToPipe(Ctx->Edge, request.release());

        auto response = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>();
        UNIT_ASSERT_VALUES_EQUAL(response->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK);
        const auto& result = response->Record.GetReadRangeResult(0);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), static_cast<ui32>(NKikimrProto::OK));
        const auto& pair = result.GetPair(0);

        NKikimrPQ::TTabletTxInfo txInfo;
        Y_ABORT_UNLESS(txInfo.ParseFromString(pair.GetValue()));

        if (txInfo.GetExecStep() == step) {
            return;
        }
    }

    UNIT_FAIL("expected execution step " << step);
}

void TPQTabletFixture::InterceptSaveTxState(TAutoPtr<IEventHandle>& ev)
{
    bool found = false;

    TTestActorRuntimeBase::TEventFilter prev;
    auto filter = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) -> bool {
        if (auto* msg = event->CastAsLocal<TEvKeyValue::TEvRequest>()) {
            if (msg->Record.HasCookie() && (msg->Record.GetCookie() == 5)) { // WRITE_TX_COOKIE
                ev = event;
                found = true;
                return true;
            }
        }

        return false;
    };
    prev = Ctx->Runtime->SetEventFilter(filter);

    TDispatchOptions options;
    options.CustomFinalCondition = [&found]() {
        return found;
    };

    UNIT_ASSERT(Ctx->Runtime->DispatchEvents(options));
    UNIT_ASSERT(found);

    Ctx->Runtime->SetEventFilter(prev);
}

void TPQTabletFixture::SendSaveTxState(TAutoPtr<IEventHandle>& event)
{
    Ctx->Runtime->Send(event);
}

void TPQTabletFixture::WaitForTheTransactionToBeDeleted(ui64 txId)
{
    const TString key = GetTxKey(txId);

    for (size_t i = 0; i < 200; ++i) {
        auto request = std::make_unique<TEvKeyValue::TEvRequest>();
        request->Record.SetCookie(12345);
        auto cmd = request->Record.AddCmdReadRange();
        auto range = cmd->MutableRange();
        range->SetFrom(key);
        range->SetIncludeFrom(true);
        range->SetTo(key);
        range->SetIncludeTo(true);
        cmd->SetIncludeData(false);
        SendToPipe(Ctx->Edge, request.release());

        auto response = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>();
        UNIT_ASSERT_VALUES_EQUAL(response->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK);

        const auto& result = response->Record.GetReadRangeResult(0);
        if (result.GetStatus() == static_cast<ui32>(NKikimrProto::OK)) {
            Ctx->Runtime->SimulateSleep(TDuration::MilliSeconds(300));
            continue;
        }

        if (result.GetStatus() == NKikimrProto::NODATA) {
            return;
        }
    }

    UNIT_FAIL("Too many attempts");
}

TVector<TString> TPQTabletFixture::WaitForExactSupportivePartitionsCount(ui32 expectedCount) {
    for (size_t i = 0; i < 200; ++i) {
        auto result = GetSupportivePartitionsKeysFromKV();

        if (result.empty() && expectedCount == 0) {
            return result;
        } else if (expectedCount == result.size()) {
            return result;
        } else {
            Ctx->Runtime->SimulateSleep(TDuration::MilliSeconds(300));
        }
    }

    UNIT_FAIL("Too many attempts");
    return {};
}

NKikimrPQ::TTabletTxInfo TPQTabletFixture::WaitForExactTxWritesCount(ui32 expectedCount) {
    for (size_t i = 0; i < 200; ++i) {
        auto result = GetTxWritesFromKV();

        if (result.TxWritesSize() == 0 && expectedCount == 0) {
            return result;
        } else if (expectedCount == result.TxWritesSize()) {
            return result;
        } else {
            Ctx->Runtime->SimulateSleep(TDuration::MilliSeconds(300));
        }
    }

    UNIT_FAIL("Too many attempts");
    return {};
}

std::string GetSupportivePartitionKeyFrom() {
    return std::string{TKeyPrefix::EServiceType::ServiceTypeData};
}

std::string GetSupportivePartitionKeyTo() {
    return std::string{static_cast<char>(TKeyPrefix::EServiceType::ServiceTypeData + 1)};
}

TVector<TString> TPQTabletFixture::GetSupportivePartitionsKeysFromKV() {
    auto request = std::make_unique<TEvKeyValue::TEvRequest>();
    request->Record.SetCookie(12345);
    auto cmd = request->Record.AddCmdReadRange();
    auto range = cmd->MutableRange();
    range->SetFrom(GetSupportivePartitionKeyFrom());
    range->SetIncludeFrom(true);
    range->SetTo(GetSupportivePartitionKeyTo());
    range->SetIncludeTo(false);
    cmd->SetIncludeData(false);
    SendToPipe(Ctx->Edge, request.release());

    auto response = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>();
    UNIT_ASSERT_VALUES_EQUAL(response->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK);

    TVector<TString> supportivePartitionsKeys;
    const auto& result = response->Record.GetReadRangeResult(0);
    if (result.GetStatus() == static_cast<ui32>(NKikimrProto::OK)) {
        for (ui32 i = 0; i < result.PairSize(); i++) {
            supportivePartitionsKeys.emplace_back(result.GetPair(i).GetKey());
        }
        return supportivePartitionsKeys;
    } else if (result.GetStatus() == NKikimrProto::NODATA) {
        return supportivePartitionsKeys;
    } else {
        UNIT_FAIL("Unexpected status from KV tablet" << result.GetStatus());
        return {};
    }
}

NKikimrPQ::TTabletTxInfo TPQTabletFixture::GetTxWritesFromKV() {
    auto request = std::make_unique<TEvKeyValue::TEvRequest>();
    request->Record.SetCookie(12345);
    auto* cmd = request->Record.AddCmdRead();
    cmd->SetKey("_txinfo");
    SendToPipe(Ctx->Edge, request.release());

    auto response = Ctx->Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>();
    UNIT_ASSERT_VALUES_EQUAL(response->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK);

    const auto& result = response->Record.GetReadResult(0);
    if (result.GetStatus() == static_cast<ui32>(NKikimrProto::OK)) {
        NKikimrPQ::TTabletTxInfo info;
        if (!info.ParseFromString(result.GetValue())) {
            UNIT_FAIL("tx writes read error");
        }
        return info;
    } else if (result.GetStatus() == NKikimrProto::NODATA) {
        return {};
    } else {
        UNIT_FAIL("Unexpected status from KV tablet" << result.GetStatus());
        return {};
    }
}


void TPQTabletFixture::SendAppSendRsRequest(const TAppSendReadSetParams& params) {
    auto makeEv = [this, &params]() {
        TCgiParameters cgi{
            {"TabletID", ToString(Ctx->TabletId)},
            {"SendReadSet", "1"},
            {"decision", params.Predicate ? "commit" : "abort"},
            {"step", ToString(params.Step)},
            {"txId", ToString(params.TxId)},
        };
        if (params.SenderId.Defined()) {
            cgi.InsertUnescaped("senderTablet", ToString(*params.SenderId));
        } else {
            cgi.InsertUnescaped("allSenderTablets", "1");
        }
        return std::make_unique<NActors::NMon::TEvRemoteHttpInfo>(TStringBuilder() << "/app?" << cgi.Print());
    };
    Ctx->Runtime->SendToPipe(Ctx->TabletId, Ctx->Edge, makeEv().release(), 0, GetPipeConfigWithRetries());
}

void TPQTabletFixture::WaitForAppSendRsResponse(const TAppSendReadSetMatcher& matcher) {
    THolder<NMon::TEvRemoteJsonInfoRes> handle = Ctx->Runtime->GrabEdgeEvent<NMon::TEvRemoteJsonInfoRes>();
    UNIT_ASSERT(handle != nullptr);
    const TString& response = handle->Json;
    NJson::TJsonValue value;
    UNIT_ASSERT(ReadJsonTree(response, &value, false));
    if (matcher.Status.Defined()) {
        const bool resultOk = value["result"].GetStringSafe() == "OK"sv;
        UNIT_ASSERT_VALUES_EQUAL(resultOk, *matcher.Status);
    }
}

Y_UNIT_TEST_F(Parallel_Transactions_1, TPQTabletFixture)
{
    TestParallelTransactions("consumer", "consumer");
}

Y_UNIT_TEST_F(Parallel_Transactions_2, TPQTabletFixture)
{
    TestParallelTransactions("consumer-1", "consumer-2");
}

Y_UNIT_TEST_F(Single_PQTablet_And_Multiple_Partitions, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    //
    // TODO(abcdef): проверить, что в команде CmdWrite есть информация о транзакции
    //

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    //
    // TODO(abcdef): проверить, что удалена информация о транзакции
    //
}

Y_UNIT_TEST_F(PQTablet_Send_RS_With_Abort, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    //
    // TODO(abcdef): проверить, что в команде CmdWrite есть информация о транзакции
    //

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_ABORT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=22222, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});

    //
    // TODO(abcdef): проверить, что удалена информация о транзакции
    //
}

Y_UNIT_TEST_F(Partition_Send_Predicate_With_False, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=2, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    //
    // TODO(abcdef): проверить, что в команде CmdWrite есть информация о транзакции
    //

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_ABORT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=22222, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});

    //
    // TODO(abcdef): проверить, что удалена информация о транзакции
    //
}

Y_UNIT_TEST_F(DropTablet_And_Tx, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;

    StartPQWriteStateObserver();

    SendProposeTransactionRequest({.TxId=txId_1,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    SendDropTablet({.TxId=12345});

    //
    // транзакция TxId_1 будет обработана
    //
    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    WaitForPQWriteState();

    //
    // по транзакции TxId_2 получим отказ
    //
    SendProposeTransactionRequest({.TxId=txId_2,
                                  .TxOps={
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});

    SendPlanStep({.Step=100, .TxIds={txId_1}});

    WaitPlanStepAck({.Step=100, .TxIds={txId_1}}); // TEvPlanStepAck для координатора
    SendDropTablet({.TxId=67890});                 // TEvDropTable когда выполняется транзакция
    WaitPlanStepAccepted({.Step=100});

    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    //
    // ответы на TEvDropTablet будут после транзакции
    //
    WaitDropTabletReply({.Status=NKikimrProto::EReplyStatus::OK, .TxId=12345, .TabletId=Ctx->TabletId, .State=NKikimrPQ::EDropped});
    WaitDropTabletReply({.Status=NKikimrProto::EReplyStatus::OK, .TxId=67890, .TabletId=Ctx->TabletId, .State=NKikimrPQ::EDropped});
}

Y_UNIT_TEST_F(DropTablet, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    //
    // транзакций нет, ответ будет сразу
    //
    SendDropTablet({.TxId=99999});
    WaitDropTabletReply({.Status=NKikimrProto::EReplyStatus::OK, .TxId=99999, .TabletId=Ctx->TabletId, .State=NKikimrPQ::EDropped});
}

Y_UNIT_TEST_F(DropTablet_Before_Write, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId_1 = 67890;
    const ui64 txId_2 = 67891;
    const ui64 txId_3 = 67892;

    StartPQWriteStateObserver();

    //
    // TEvDropTablet между транзакциями
    //
    SendProposeTransactionRequest({.TxId=txId_1,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    SendDropTablet({.TxId=12345});
    SendProposeTransactionRequest({.TxId=txId_2,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"}
                                  }});

    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    WaitForPQWriteState();

    SendProposeTransactionRequest({.TxId=txId_3,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=1, .Consumer="user", .Begin=0, .End=0, .Path="/topic"}
                                  }});

    //
    // транзакция пришла до того как состояние было записано на диск. будет обработана
    //
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    //
    // транзакция пришла после того как состояние было записано на диск. не будет обработана
    //
    WaitProposeTransactionResponse({.TxId=txId_3,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(DropTablet_And_UnplannedConfigTransaction, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId = 67890;

    auto tabletConfig =
        NHelpers::MakeConfig(2, {
                             {.Consumer="client-1", .Generation=0},
                             {.Consumer="client-3", .Generation=7}},
                             2);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    // The 'TEvDropTablet` message arrives when the transaction has not yet received a PlanStep. We know that SS
    // performs no more than one operation at a time. Therefore, we believe that no one is waiting for this
    // transaction anymore.
    SendDropTablet({.TxId=12345});
    WaitDropTabletReply({.Status=NKikimrProto::EReplyStatus::OK, .TxId=12345, .TabletId=Ctx->TabletId, .State=NKikimrPQ::EDropped});
}

Y_UNIT_TEST_F(DropTablet_And_PlannedConfigTransaction, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId = 67890;

    auto tabletConfig =
        NHelpers::MakeConfig(2, {
                             {.Consumer="client-1", .Generation=0},
                             {.Consumer="client-3", .Generation=7}},
                             2);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});
    WaitPlanStepAck({.Step=100, .TxIds={txId}});

    // The 'TEvDropTablet` message arrives when the transaction has already received a PlanStep.
    // We will receive the response when the transaction is executed.
    SendDropTablet({.TxId=12345});

    WaitPlanStepAccepted({.Step=100});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    WaitDropTabletReply({.Status=NKikimrProto::EReplyStatus::OK, .TxId=12345, .TabletId=Ctx->TabletId, .State=NKikimrPQ::EDropped});
}

Y_UNIT_TEST_F(UpdateConfig_1, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId = 67890;

    auto tabletConfig =
        NHelpers::MakeConfig(2, {
                             {.Consumer="client-1", .Generation=0},
                             {.Consumer="client-3", .Generation=7}},
                             2);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}});
    WaitPlanStepAccepted({.Step=100});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

Y_UNIT_TEST_F(UpdateConfig_2, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId_2 = 67891;
    const ui64 txId_3 = 67892;

    auto tabletConfig =
        NHelpers::MakeConfig(2, {
                             {.Consumer="client-1", .Generation=1},
                             {.Consumer="client-2", .Generation=1}
                             },
                             3);

    SendProposeTransactionRequest({.TxId=txId_2,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    SendProposeTransactionRequest({.TxId=txId_3,
                                  .TxOps={
                                  {.Partition=1, .Consumer="client-2", .Begin=0, .End=0, .Path="/topic"},
                                  {.Partition=2, .Consumer="client-1", .Begin=0, .End=0, .Path="/topic"}
                                  }});

    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});
    WaitProposeTransactionResponse({.TxId=txId_3,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId_2, txId_3}});

    WaitPlanStepAck({.Step=100, .TxIds={txId_2, txId_3}});
    WaitPlanStepAccepted({.Step=100});

    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
    WaitProposeTransactionResponse({.TxId=txId_3,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

void TPQTabletFixture::TestWaitingForTEvReadSet(size_t sendersCount, size_t receiversCount)
{
    const ui64 txId = 67890;

    TVector<NHelpers::TPQTabletMock*> tablets;
    TVector<ui64> senders;
    TVector<ui64> receivers;

    //
    // senders
    //
    for (size_t i = 0; i < sendersCount; ++i) {
        senders.push_back(22222 + i);
        tablets.push_back(CreatePQTabletMock(senders.back()));
    }

    //
    // receivers
    //
    for (size_t i = 0; i < receiversCount; ++i) {
        receivers.push_back(33333 + i);
        tablets.push_back(CreatePQTabletMock(receivers.back()));
    }

    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders=senders, .Receivers=receivers,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"}
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForCalcPredicateResult();

    //
    // The tablet received the predicate value from the partition, but has not yet saved the transaction state.
    // Therefore, the transaction has not yet entered the WAIT_RS state
    //

    for (size_t i = 0; i < sendersCount; ++i) {
        tablets[i]->SendReadSet(*Ctx->Runtime,
                                {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});
    }

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

Y_UNIT_TEST_F(Test_Waiting_For_TEvReadSet_When_There_Are_More_Senders_Than_Recipients, TPQTabletFixture)
{
    TestWaitingForTEvReadSet(4, 2);
}

Y_UNIT_TEST_F(Test_Waiting_For_TEvReadSet_When_There_Are_Fewer_Senders_Than_Recipients, TPQTabletFixture)
{
    TestWaitingForTEvReadSet(2, 4);
}

Y_UNIT_TEST_F(Test_Waiting_For_TEvReadSet_When_The_Number_Of_Senders_And_Recipients_Match, TPQTabletFixture)
{
    TestWaitingForTEvReadSet(2, 2);
}

Y_UNIT_TEST_F(Test_Waiting_For_TEvReadSet_Without_Recipients, TPQTabletFixture)
{
    TestWaitingForTEvReadSet(2, 0);
}

Y_UNIT_TEST_F(Test_Waiting_For_TEvReadSet_Without_Senders, TPQTabletFixture)
{
    TestWaitingForTEvReadSet(0, 2);
}

Y_UNIT_TEST_F(TEvReadSet_comes_before_TEvPlanStep, TPQTabletFixture)
{
    const ui64 mockTabletId = 22222;

    CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=1, .Path="/topic"}
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendReadSet({.Step=100, .TxId=txId, .Source=mockTabletId, .Target=Ctx->TabletId, .Predicate=true});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});
}

Y_UNIT_TEST_F(Cancel_Tx, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    StartPQWriteTxsObserver();

    SendCancelTransactionProposal({.TxId=txId});

    WaitForPQWriteTxs();
}

Y_UNIT_TEST_F(ProposeTx_Missing_Operations, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 2;

    SendProposeTransactionRequest({.TxId=txId,
                                  });
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(ProposeTx_Unknown_Partition_1, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 2;
    const ui32 unknownPartitionId = 3;

    SendProposeTransactionRequest({.TxId=txId,
                                  .TxOps={{.Partition=unknownPartitionId, .Path="/topic"}}
                                  });
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(ProposeTx_Unknown_WriteId, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 2;
    const TWriteId writeId(0, 3);

    SendProposeTransactionRequest({.TxId=txId,
                                  .TxOps={{.Partition=0, .Path="/topic"}},
                                  .WriteId=writeId});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(ProposeTx_Unknown_Partition_2, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=2}, {}, *Ctx);

    const ui64 txId = 2;
    const TWriteId writeId(0, 3);
    const ui64 cookie = 4;

    SendGetOwnershipRequest({.Partition=0,
                            .WriteId=writeId,
                            .Owner=DEFAULT_OWNER,
                            .Cookie=cookie});
    WaitGetOwnershipResponse({.Cookie=cookie});

    SendProposeTransactionRequest({.TxId=txId,
                                  .TxOps={{.Partition=1, .Path="/topic"}},
                                  .WriteId=writeId});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(ProposeTx_Command_After_Propose, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui32 partitionId = 0;
    const ui64 txId = 2;
    const TWriteId writeId(0, 3);

    SyncGetOwnership({.Partition=partitionId,
                     .WriteId=writeId,
                     .NeedSupportivePartition=true,
                     .Owner=DEFAULT_OWNER,
                     .Cookie=4},
                     {.Cookie=4,
                     .Status=NMsgBusProxy::MSTATUS_OK});

    SendProposeTransactionRequest({.TxId=txId,
                                  .TxOps={{.Partition=partitionId, .Path="/topic", .SupportivePartition=100'000}},
                                  .WriteId=writeId});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SyncGetOwnership({.Partition=partitionId,
                     .WriteId=writeId,
                     .Owner=DEFAULT_OWNER,
                     .Cookie=5},
                     {.Cookie=5,
                     .Status=NMsgBusProxy::MSTATUS_ERROR});
}

Y_UNIT_TEST_F(Read_TEvTxCommit_After_Restart, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForCalcPredicateResult();

    // the transaction is now in the WAIT_RS state in memory and PLANNED state in disk

    PQTabletRestart(*Ctx);

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=mockTabletId, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});
}

Y_UNIT_TEST_F(Config_TEvTxCommit_After_Restart, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    auto tabletConfig = NHelpers::MakeConfig({.Version=2,
                                             .Consumers={
                                             {.Consumer="client-1", .Generation=0},
                                             {.Consumer="client-3", .Generation=7}
                                             },
                                             .Partitions={
                                             {.Id=0}
                                             },
                                             .AllPartitions={
                                             {.Id=0, .TabletId=Ctx->TabletId, .Children={},  .Parents={1}},
                                             {.Id=1, .TabletId=mockTabletId,  .Children={0}, .Parents={}}
                                             }});

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForProposePartitionConfigResult();

    // the transaction is now in the WAIT_RS state in memory and PLANNED state in disk

    PQTabletRestart(*Ctx);

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=mockTabletId, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});
}

Y_UNIT_TEST_F(One_Tablet_For_All_Partitions, TPQTabletFixture)
{
    const ui64 txId = 67890;

    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    auto tabletConfig = NHelpers::MakeConfig({.Version=2,
                                             .Consumers={
                                             {.Consumer="client-1", .Generation=0},
                                             {.Consumer="client-3", .Generation=7}
                                             },
                                             .Partitions={
                                             {.Id=0},
                                             {.Id=1},
                                             {.Id=2}
                                             },
                                             .AllPartitions={
                                             {.Id=0, .TabletId=Ctx->TabletId, .Children={1, 2},  .Parents={}},
                                             {.Id=1, .TabletId=Ctx->TabletId, .Children={}, .Parents={0}},
                                             {.Id=2, .TabletId=Ctx->TabletId, .Children={}, .Parents={0}}
                                             }});

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForProposePartitionConfigResult(2);

    // the transaction is now in the WAIT_RS state in memory and PLANNED state in disk

    PQTabletRestart(*Ctx);

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

Y_UNIT_TEST_F(One_New_Partition_In_Another_Tablet, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    auto tabletConfig = NHelpers::MakeConfig({.Version=2,
                                             .Consumers={
                                             {.Consumer="client-1", .Generation=0},
                                             {.Consumer="client-3", .Generation=7}
                                             },
                                             .Partitions={
                                             {.Id=0},
                                             {.Id=1},
                                             },
                                             .AllPartitions={
                                             {.Id=0, .TabletId=Ctx->TabletId, .Children={1, 2}, .Parents={}},
                                             {.Id=1, .TabletId=Ctx->TabletId, .Children={}, .Parents={0}},
                                             {.Id=2, .TabletId=mockTabletId,  .Children={}, .Parents={0}}
                                             }});

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForProposePartitionConfigResult(2);

    // the transaction is now in the WAIT_RS state in memory and PLANNED state in disk

    PQTabletRestart(*Ctx);

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=mockTabletId, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});
}

Y_UNIT_TEST_F(All_New_Partitions_In_Another_Tablet, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    auto tabletConfig = NHelpers::MakeConfig({.Version=2,
                                             .Consumers={
                                             {.Consumer="client-1", .Generation=0},
                                             {.Consumer="client-3", .Generation=7}
                                             },
                                             .Partitions={
                                             {.Id=0},
                                             {.Id=1},
                                             },
                                             .AllPartitions={
                                             {.Id=0, .TabletId=Ctx->TabletId, .Children={}, .Parents={2}},
                                             {.Id=1, .TabletId=Ctx->TabletId, .Children={}, .Parents={2}},
                                             {.Id=2, .TabletId=mockTabletId,  .Children={0, 1}, .Parents={}}
                                             }});

    SendProposeTransactionRequest({.TxId=txId,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForProposePartitionConfigResult(2);

    // the transaction is now in the WAIT_RS state in memory and PLANNED state in disk

    PQTabletRestart(*Ctx);

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    tablet->SendReadSetAck(*Ctx->Runtime, {.Step=100, .TxId=txId, .Source=Ctx->TabletId});
    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=mockTabletId, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});
}

Y_UNIT_TEST_F(Huge_ProposeTransacton, TPQTabletFixture)
{
    const ui64 mockTabletId = 22222;

    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    auto tabletConfig = NHelpers::MakeConfig({.Version=2,
                                             .Consumers={
                                             {.Consumer="client-1", .Generation=0},
                                             {.Consumer="client-3", .Generation=7},
                                             },
                                             .Partitions={
                                             {.Id=0},
                                             {.Id=1},
                                             },
                                             .AllPartitions={
                                             {.Id=0, .TabletId=Ctx->TabletId, .Children={}, .Parents={2}},
                                             {.Id=1, .TabletId=Ctx->TabletId, .Children={}, .Parents={2}},
                                             {.Id=2, .TabletId=mockTabletId,  .Children={0, 1}, .Parents={}}
                                             },
                                             .HugeConfig = true});

    const ui64 txId_1 = 67890;
    SendProposeTransactionRequest({.TxId=txId_1,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    const ui64 txId_2 = 67891;
    SendProposeTransactionRequest({.TxId=txId_2,
                                  .Configs=NHelpers::TConfigParams{
                                  .Tablet=tabletConfig,
                                  .Bootstrap=NHelpers::MakeBootstrapConfig(),
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    PQTabletRestart(*Ctx);
    ResetPipe();

    SendPlanStep({.Step=100, .TxIds={txId_1, txId_2}});
    WaitPlanStepAck({.Step=100, .TxIds={txId_1, txId_2}});
    WaitPlanStepAccepted({.Step=100});
}

Y_UNIT_TEST_F(After_Restarting_The_Tablet_Sends_A_TEvReadSet_For_Transactions_In_The_EXECUTED_State, TPQTabletFixture)
{
    const ui64 txId_1 = 67890;
    const ui64 txId_2 = txId_1 + 1;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    // 1st tx
    SendProposeTransactionRequest({.TxId=txId_1,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId_1}});

    WaitForCalcPredicateResult();

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId_1, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId_1,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    WaitForTxState(txId_1, NKikimrPQ::TTransaction::EXECUTED);

    tablet->ReadSet = Nothing();

    // 2nd tx
    SendProposeTransactionRequest({.TxId=txId_2,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId_2,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=110, .TxIds={txId_2}});

    WaitForCalcPredicateResult();

    WaitReadSetEx(*tablet, {.Step=110, .TxId=txId_2, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Count=1});

    // the PQ tablet has moved a step forward
    WaitForExecStep(110);

    // restart PQ tablet
    PQTabletRestart(*Ctx);

    // the PQ tablet should send a TEvReadSet for the executed transaction
    WaitReadSetEx(*tablet, {.Step=100, .TxId=txId_1, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Count=2});
}

Y_UNIT_TEST_F(TEvReadSet_Is_Not_Sent_Ahead_Of_Time, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = 22222;

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitForCalcPredicateResult();

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    //WaitProposeTransactionResponse({.TxId=txId,
    //                               .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    TAutoPtr<IEventHandle> kvRequest;
    InterceptSaveTxState(kvRequest);

    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitForNoReadSetAck(*tablet);

    SendSaveTxState(kvRequest);

    WaitForTxState(txId, NKikimrPQ::TTransaction::EXECUTED);

    WaitReadSetAck(*tablet, {.Step=100, .TxId=txId, .Source=22222, .Target=Ctx->TabletId, .Consumer=Ctx->TabletId});
}

Y_UNIT_TEST_F(TEvReadSet_For_A_Non_Existent_Tablet, TPQTabletFixture)
{
    const ui64 txId = 67890;
    const ui64 mockTabletId = MakeTabletID(false, 22222);

    // We are simulating a situation where the recipient of TEvReadSet has already completed a transaction
    // and has been deleted.
    //
    // To do this, we "forget" the TEvReadSet from the PQ tablet and send TEvClientConnected with the Dead flag
    // instead of TEvReadSetAck.
    TTestActorRuntimeBase::TEventFilter prev;
    auto filter = [&](TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) -> bool {
        if (auto* msg = event->CastAsLocal<TEvTxProcessing::TEvReadSet>()) {
            const auto& r = msg->Record;
            if (r.GetTabletSource() == Ctx->TabletId) {
                runtime.Send(event->Sender,
                             Ctx->Edge,
                             new TEvTabletPipe::TEvClientConnected(mockTabletId,
                                                                   NKikimrProto::ERROR,
                                                                   event->Sender,
                                                                   TActorId(),
                                                                   true,
                                                                   true, // Dead
                                                                   0));
                return true;
            }
        }
        return false;
    };
    prev = Ctx->Runtime->SetEventFilter(filter);

    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(mockTabletId);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={mockTabletId}, .Receivers={mockTabletId},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    // We are sending a TEvReadSet so that the PQ tablet can complete the transaction.
    tablet->SendReadSet(*Ctx->Runtime,
                        {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    WaitProposeTransactionResponse({.TxId=txId, .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});

    // Instead of TEvReadSetAck, the PQ tablet will receive TEvClientConnected with the Dead flag. The transaction
    // will switch from the WAIT_RS_AKS state to the DELETING state.
    WaitForTheTransactionToBeDeleted(txId);
}

Y_UNIT_TEST_F(Limit_On_The_Number_Of_Transactons, TPQTabletFixture)
{
    const ui64 mockTabletId = MakeTabletID(false, 22222);
    const ui64 txId = 67890;

    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    for (ui64 i = 0; i < 1002; ++i) {
        SendProposeTransactionRequest({.TxId=txId + i,
                                      .Senders={mockTabletId}, .Receivers={mockTabletId},
                                      .TxOps={
                                      {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                      }});
    }

    size_t preparedCount = 0;
    size_t overloadedCount = 0;

    for (ui64 i = 0; i < 1002; ++i) {
        auto event = Ctx->Runtime->GrabEdgeEvent<TEvPersQueue::TEvProposeTransactionResult>();
        UNIT_ASSERT(event != nullptr);

        UNIT_ASSERT(event->Record.HasStatus());

        const auto status = event->Record.GetStatus();
        switch (status) {
        case NKikimrPQ::TEvProposeTransactionResult::PREPARED:
            ++preparedCount;
            break;
        case NKikimrPQ::TEvProposeTransactionResult::OVERLOADED:
            ++overloadedCount;
            break;
        default:
            UNIT_FAIL("unexpected transaction status " << NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(status));
        }
    }

    UNIT_ASSERT_EQUAL(preparedCount, 1000);
    UNIT_ASSERT_EQUAL(overloadedCount, 2);
}

Y_UNIT_TEST_F(Kafka_Transaction_Supportive_Partitions_Should_Be_Deleted_After_Timeout, TPQTabletFixture)
{
    NKafka::TProducerInstanceId producerInstanceId = {1, 0};
    PQTabletPrepare({.partitions=1}, {}, *Ctx);
    EnsurePipeExist();
    TString ownerCookie = CreateSupportivePartitionForKafka(producerInstanceId);

    // send data to create blobs for supportive partitions
    SendKafkaTxnWriteRequest(producerInstanceId, ownerCookie);

    // validate supportive partition was created
    WaitForExactSupportivePartitionsCount(1);
    auto txInfo = GetTxWritesFromKV();
    UNIT_ASSERT_VALUES_EQUAL(txInfo.TxWritesSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(txInfo.GetTxWrites(0).GetKafkaTransaction(), true);

    // increment time till after kafka txn timeout
    ui64 kafkaTxnTimeoutMs = Ctx->Runtime->GetAppData(0).KafkaProxyConfig.GetTransactionTimeoutMs()
        + KAFKA_TRANSACTION_DELETE_DELAY_MS;
    Ctx->Runtime->AdvanceCurrentTime(TDuration::MilliSeconds(kafkaTxnTimeoutMs + 1));
    SendToPipe(Ctx->Edge, MakeHolder<TEvents::TEvWakeup>().Release());

    // wait till supportive partition for this kafka transaction is deleted
    WaitForExactSupportivePartitionsCount(0);
    // validate that TxWrite for this transaction is deleted
    auto txInfo2 = GetTxWritesFromKV();
    UNIT_ASSERT_VALUES_EQUAL(txInfo2.TxWritesSize(), 0);
}

Y_UNIT_TEST_F(Non_Kafka_Transaction_Supportive_Partitions_Should_Not_Be_Deleted_After_Timeout, TPQTabletFixture)
{
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    // create Topic API transaction
    SyncGetOwnership({.Partition=0,
                     .WriteId=TWriteId{0, 3},
                     .NeedSupportivePartition=true,
                     .Owner=DEFAULT_OWNER,
                     .Cookie=4},
                     {.Cookie=4,
                     .Status=NMsgBusProxy::MSTATUS_OK});
    auto txInfo = GetTxWritesFromKV();
    UNIT_ASSERT_VALUES_EQUAL(txInfo.TxWritesSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(txInfo.GetTxWrites(0).GetKafkaTransaction(), false);

    // create Kafka transaction
    CreateSupportivePartitionForKafka({1, 0});
    auto txInfo2 = GetTxWritesFromKV();
    UNIT_ASSERT_VALUES_EQUAL(txInfo2.TxWritesSize(), 2);

    // increment time till after kafka txn timeout
    ui64 kafkaTxnTimeoutMs = Ctx->Runtime->GetAppData(0).KafkaProxyConfig.GetTransactionTimeoutMs()
        + KAFKA_TRANSACTION_DELETE_DELAY_MS;
    Ctx->Runtime->AdvanceCurrentTime(TDuration::MilliSeconds(kafkaTxnTimeoutMs + 1));
    SendToPipe(Ctx->Edge, MakeHolder<TEvents::TEvWakeup>().Release());

    // wait till supportive partition for this kafka transaction is deleted
    auto txInfo3 = WaitForExactTxWritesCount(1);
    UNIT_ASSERT_VALUES_EQUAL(txInfo3.GetTxWrites(0).GetKafkaTransaction(), false);
}

Y_UNIT_TEST_F(In_Kafka_Txn_Only_Supportive_Partitions_That_Exceeded_Timeout_Should_Be_Deleted, TPQTabletFixture)
{
    NKafka::TProducerInstanceId producerInstanceId1 = {1, 0};
    NKafka::TProducerInstanceId producerInstanceId2 = {2, 0};
    PQTabletPrepare({.partitions=1}, {}, *Ctx);
    EnsurePipeExist();

    // create first kafka-transacition and write data to it
    TString ownerCookie1 = CreateSupportivePartitionForKafka(producerInstanceId1);
    SendKafkaTxnWriteRequest(producerInstanceId1, ownerCookie1);
    WaitForExactSupportivePartitionsCount(1);
    ResetPipe();

    // advance time to value strictly less then kafka transaction timeout
    ui64 testTimeAdvanceMs = KAFKA_TRANSACTION_DELETE_DELAY_MS / 2;
    Ctx->Runtime->AdvanceCurrentTime(TDuration::MilliSeconds(testTimeAdvanceMs));

    // create second kafka-transacition and write data to it
    EnsurePipeExist();
    TString ownerCookie2 = CreateSupportivePartitionForKafka(producerInstanceId2);
    SendKafkaTxnWriteRequest(producerInstanceId2, ownerCookie2);
    WaitForExactSupportivePartitionsCount(2);

    // increment time till after timeout for the first transaction
    Ctx->Runtime->AdvanceCurrentTime(TDuration::MilliSeconds(
        Ctx->Runtime->GetAppData(0).KafkaProxyConfig.GetTransactionTimeoutMs() + testTimeAdvanceMs + 1));
    // trigger expired transactions cleanup
    SendToPipe(Ctx->Edge, MakeHolder<TEvents::TEvWakeup>().Release());

    // wait till supportive partition for first kafka transaction is deleted
    WaitForExactSupportivePartitionsCount(1);
    // validate that TxWrite for first transaction is deleted and for the second is preserved
    auto txInfo = GetTxWritesFromKV();
    UNIT_ASSERT_EQUAL(txInfo.TxWritesSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(txInfo.GetTxWrites(0).GetWriteId().GetKafkaProducerInstanceId().GetId(), producerInstanceId2.Id);
}

void TPQTabletFixture::TestSendingTEvReadSetViaApp(const TSendReadSetViaAppTestParams& params)
{
    Y_ABORT_UNLESS(params.TabletsRSCount <= params.TabletsCount);
    const ui64 txId = 67890;

    TVector<NHelpers::TPQTabletMock*> tablets;
    TVector<ui64> tabletIds;
    for (size_t i = 0; i < params.TabletsCount; ++i) {
        tabletIds.push_back(22222 + i);
        tablets.push_back(CreatePQTabletMock(tabletIds.back()));
    }

    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders=tabletIds, .Receivers=tabletIds,
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"}
                                  }});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});
    WaitPlanStepAccepted({.Step=100});

    for (auto* tablet : tablets) {
        WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=tablet->TabletID(), .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    }
    for (size_t i = 0; i < Min(params.TabletsRSCount, params.TabletsCount); ++i) {
        tablets[i]->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=params.Decision});
    }
    Ctx->Runtime->SimulateSleep(TDuration::MilliSeconds(500));

    SendAppSendRsRequest({.Step=100, .TxId=txId, .SenderId=Nothing(), .Predicate=(params.AppDecision == NKikimrTx::TReadSetData::DECISION_COMMIT),});
    WaitForAppSendRsResponse({.Status = params.ExpectedAppResponseStatus,});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=params.ExpectedStatus});
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c0c, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 0,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::COMPLETE,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c3c, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 3,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::COMPLETE,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c5c, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 5,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .ExpectedAppResponseStatus = false,  // получены все RS до вызова app
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::COMPLETE,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c0a, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 0,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::ABORTED,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c3a, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 3,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::ABORTED,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5c5a, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .TabletsRSCount = 5,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .ExpectedAppResponseStatus = false,  // получены все RS до вызова app
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::COMPLETE,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5a4c, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .TabletsRSCount = 4,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_COMMIT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::ABORTED,
    });
}

Y_UNIT_TEST_F(PQTablet_Send_ReadSet_Via_App_5a4a, TPQTabletFixture)
{
    TestSendingTEvReadSetViaApp({
        .TabletsCount = 5,
        .Decision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .TabletsRSCount = 4,
        .AppDecision = NKikimrTx::TReadSetData::DECISION_ABORT,
        .ExpectedAppResponseStatus = true,
        .ExpectedStatus = NKikimrPQ::TEvProposeTransactionResult::ABORTED,
    });
}

Y_UNIT_TEST_F(PQTablet_App_SendReadSet_With_Commit, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});

    SendAppSendRsRequest({.Step=100, .TxId=txId, .SenderId=22222, .Predicate=true,});
    WaitForAppSendRsResponse({.Status = true,});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::COMPLETE});
}

Y_UNIT_TEST_F(PQTablet_App_SendReadSet_With_Abort, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});

    SendAppSendRsRequest({.Step=100, .TxId=txId, .SenderId=22222, .Predicate=false,});
    WaitForAppSendRsResponse({.Status = true,});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}

Y_UNIT_TEST_F(PQTablet_App_SendReadSet_With_Commit_After_Abort, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_ABORT});

    SendAppSendRsRequest({.Step=100, .TxId=txId, .SenderId=22222, .Predicate=true,});
    WaitForAppSendRsResponse({.Status = true,});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED});
}


Y_UNIT_TEST_F(PQTablet_App_SendReadSet_With_Abort_After_Commit, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT});

    SendAppSendRsRequest({.Step=100, .TxId=txId, .SenderId=22222, .Predicate=false,});
    WaitForAppSendRsResponse({.Status = true,});
    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::ABORTED}); // RS=commit + ручной abort -> abort
}

Y_UNIT_TEST_F(PQTablet_App_SendReadSet_Invalid_Tx, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});

    SendAppSendRsRequest({.Step=100, .TxId=txId+1, .SenderId=22222, .Predicate=true,});
    WaitForAppSendRsResponse({.Status = false,});
}

Y_UNIT_TEST_F(PQTablet_App_SendReadSet_Invalid_Step, TPQTabletFixture)
{
    NHelpers::TPQTabletMock* tablet = CreatePQTabletMock(22222);
    PQTabletPrepare({.partitions=1}, {}, *Ctx);

    const ui64 txId = 67890;

    SendProposeTransactionRequest({.TxId=txId,
                                  .Senders={22222}, .Receivers={22222},
                                  .TxOps={
                                  {.Partition=0, .Consumer="user", .Begin=0, .End=0, .Path="/topic"},
                                  }});

    WaitProposeTransactionResponse({.TxId=txId,
                                   .Status=NKikimrPQ::TEvProposeTransactionResult::PREPARED});

    SendPlanStep({.Step=100, .TxIds={txId}});

    WaitPlanStepAck({.Step=100, .TxIds={txId}}); // TEvPlanStepAck для координатора
    WaitPlanStepAccepted({.Step=100});

    WaitReadSet(*tablet, {.Step=100, .TxId=txId, .Source=Ctx->TabletId, .Target=22222, .Decision=NKikimrTx::TReadSetData::DECISION_COMMIT, .Producer=Ctx->TabletId});
    tablet->SendReadSet(*Ctx->Runtime, {.Step=100, .TxId=txId, .Target=Ctx->TabletId, .Decision=NKikimrTx::TReadSetData::DECISION_ABORT});

    SendAppSendRsRequest({.Step=101, .TxId=txId, .SenderId=22222, .Predicate=true,});
    WaitForAppSendRsResponse({.Status = false,});
}


}

}
