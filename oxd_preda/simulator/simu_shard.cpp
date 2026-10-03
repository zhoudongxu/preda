#include "../../SFC/core/ext/botan/botan.h"
#include "../../SFC/core/ext/bignum/big_num.h"
#include "simu_global.h"
#include "chain_simu.h"

#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
#include "../runtime/relay_plan/RelayPlanLoader.h"
#include <algorithm>
#endif

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
#include "relay_optimization/RelayReservePlanner.h"

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <vector>
#endif

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
#include "relay_trace/RelayManifestLoader.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#endif

#if defined(__linux__) && defined(AFFINITY_SET)
#include <pthread.h>
#include <sched.h>
#endif


namespace oxd
{

#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
namespace
{

void PopulateRelayOriginMetadata(
	SimuTxn* txn,
	const std::string& moduleId,
	const std::string& functionId,
	uint32_t sourceOpcode,
	uint32_t siteOrdinal,
	const std::string& siteId) noexcept
{
	if(txn == nullptr || moduleId.empty() || functionId.empty())
		return;
	if(siteOrdinal == relay_plan::InvalidRelaySiteOrdinal)
		return;
	const size_t moduleSize = std::min(
		moduleId.size(),
		SimuTxn::RelayOriginModuleCapacity - 1);
	const size_t functionSize = std::min(
		functionId.size(),
		SimuTxn::RelayOriginFunctionCapacity - 1);
	const size_t siteSize = std::min(
		siteId.size(),
		SimuTxn::RelayOriginSiteCapacity - 1);
	if(moduleSize == 0 || functionSize == 0 ||
		moduleSize > std::numeric_limits<uint16_t>::max() ||
		functionSize > std::numeric_limits<uint16_t>::max() ||
		siteSize > std::numeric_limits<uint16_t>::max())
		return;
	memcpy(txn->RelayOriginModule, moduleId.data(), moduleSize);
	memcpy(txn->RelayOriginFunction, functionId.data(), functionSize);
	if(siteSize != 0)
		memcpy(txn->RelayOriginSite, siteId.data(), siteSize);
	txn->RelayOriginModule[moduleSize] = '\0';
	txn->RelayOriginFunction[functionSize] = '\0';
	txn->RelayOriginSite[siteSize] = '\0';
	txn->RelayOriginModuleSize = static_cast<uint16_t>(moduleSize);
	txn->RelayOriginFunctionSize = static_cast<uint16_t>(functionSize);
	txn->RelayOriginSiteSize = static_cast<uint16_t>(siteSize);
	txn->RelayOriginOpcode = sourceOpcode;
	txn->RelayOriginSiteOrdinal = siteOrdinal;
	txn->RelayOriginMetadataValid = 1;
}

} // namespace
#endif

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
namespace
{

uint64_t RelayOptimizationNowNs() noexcept
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

void AppendBigEndian(
	uint8_t* output,
	size_t& offset,
	uint64_t value,
	size_t width) noexcept
{
	for(size_t index = 0; index < width; ++index)
	{
		const size_t shift = (width - index - 1) * 8;
		output[offset++] =
			static_cast<uint8_t>((value >> shift) & 0xff);
	}
}

uint64_t RelayAuditRootIdentity(
	relay_optimization::RelayOptimizationAudit& audit,
	const SimuTxn& transaction) noexcept
{
	const rvm::ConstData arguments = transaction.GetArguments();
	std::array<uint8_t,
		1 + sizeof(transaction.Contract) +
		sizeof(transaction.Op) +
		sizeof(transaction.Target.target_size)> prefix{};
	size_t prefixSize = 0;
	AppendBigEndian(
		prefix.data(),
		prefixSize,
		static_cast<uint64_t>(transaction.Type),
		1);
	AppendBigEndian(
		prefix.data(),
		prefixSize,
		static_cast<uint64_t>(transaction.Contract),
		sizeof(transaction.Contract));
	AppendBigEndian(
		prefix.data(),
		prefixSize,
		static_cast<uint64_t>(transaction.Op),
		sizeof(transaction.Op));
	AppendBigEndian(
		prefix.data(),
		prefixSize,
		transaction.Target.target_size,
		sizeof(transaction.Target.target_size));
	const uint8_t* target = reinterpret_cast<const uint8_t*>(
		&transaction.Target.u512);
	std::array<uint8_t, sizeof(arguments.DataSize)> argumentSize{};
	size_t argumentSizeBytes = 0;
	AppendBigEndian(
		argumentSize.data(),
		argumentSizeBytes,
		arguments.DataSize,
		sizeof(arguments.DataSize));
	const uint8_t* argumentBytes =
		static_cast<const uint8_t*>(arguments.DataPtr);
	// Normal invocations do not carry a separate actor field. Their effective
	// target is already represented by target_size plus the exact target
	// bytes above. Do not hash the full Target.addr union for non-address
	// scopes: bytes outside target_size have no source-level meaning.
	const uint8_t* relayInitiator = transaction.IsRelay()
		? reinterpret_cast<const uint8_t*>(
			&transaction.Initiator)
		: nullptr;
	const relay_optimization::AuditIdentityPart parts[] = {
		{prefix.data(), prefixSize},
		{target, transaction.Target.target_size},
		{argumentSize.data(), argumentSizeBytes},
		{argumentBytes,
			argumentBytes
				? static_cast<size_t>(arguments.DataSize)
				: 0},
		{relayInitiator,
			relayInitiator
				? sizeof(transaction.Initiator)
				: 0},
	};
	return audit.StableRootIdentity(parts, std::size(parts));
}

} // namespace
#endif

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
namespace
{

relay_trace::ScopeKind GetRelayTraceScopeKind(rvm::Scope scope)
{
	switch (scope)
	{
	case rvm::Scope::Global:
		return relay_trace::ScopeKind::Global;
	case rvm::Scope::Shard:
		return relay_trace::ScopeKind::Shard;
	case rvm::Scope::Address:
		return relay_trace::ScopeKind::Address;
	default:
		break;
	}

	const auto keyType = static_cast<rvm::ScopeKeySized>(
		static_cast<uint8_t>(rvm::SCOPE_KEYSIZETYPE(scope)) &
		static_cast<uint8_t>(rvm::ScopeKeySized::BaseTypeBitmask));
	switch (keyType)
	{
	case rvm::ScopeKeySized::Address:
		return relay_trace::ScopeKind::Address;
	case rvm::ScopeKeySized::UInt32:
		return relay_trace::ScopeKind::Uint32;
	case rvm::ScopeKeySized::UInt64:
		return relay_trace::ScopeKind::Uint64;
	case rvm::ScopeKeySized::UInt96:
		return relay_trace::ScopeKind::Uint96;
	case rvm::ScopeKeySized::UInt128:
		return relay_trace::ScopeKind::Uint128;
	case rvm::ScopeKeySized::UInt160:
		return relay_trace::ScopeKind::Uint160;
	case rvm::ScopeKeySized::UInt256:
		return relay_trace::ScopeKind::Uint256;
	case rvm::ScopeKeySized::UInt512:
		return relay_trace::ScopeKind::Uint512;
	default:
		return relay_trace::ScopeKind::Unknown;
	}
}

std::string GetRelayTraceModuleIdentity(
	const rvm::ContractModuleID& module)
{
	return relay_trace::RelayManifestLoader::RuntimeHashIdentity(module);
}

template<typename Observer>
void RunRelayTraceObservation(
	relay_trace::RelayTraceCollector* collector,
	const char* operation,
	Observer&& observer) noexcept
{
	try
	{
		observer();
	}
	catch (...)
	{
		// Runtime tracing is observational. Allocation, parsing, reporting,
		// or validation failures must never alter the transaction path.
		if (collector != nullptr)
			collector->RecordObserverFailureNoexcept(operation);
	}
}

} // namespace
#endif

void ShardStates::Revert()
{
	_ShardStates.Empty();
	_ShardKeyedStates.Empty();
}

void ShardStates::Commit(ShardStates& to)
{
	_ShardStates.Commit(to._ShardStates);
	_ShardKeyedStates.Commit(to._ShardKeyedStates);
}

SimuShard::SimuShard(ChainSimulator* simu, uint64_t time_base, uint32_t shard_order)
	:_TxnEmitted(simu, this)
{
	_pSimulator = simu;
	_pGlobalShard = nullptr;

	_BlockTimeBase = time_base;
	_ShardIndex = rvm::GlobalShard;
	_ShardOrder = shard_order;
	_GoNextBlock.Reset();
	rt::Zero(_PrevBlockHash);
}

SimuShard::SimuShard(ChainSimulator* simu, uint64_t time_base, uint32_t shard_order, uint32_t shard_index, SimuGlobalShard* global)
	:SimuShard(simu, time_base, shard_order)
{
	_pGlobalShard = global;
	_ShardIndex = shard_index;
}

rvm::DAppId SimuShard::GetDAppByName(const rvm::ConstString* dapp_name) const
{
	return _pGlobalShard->_GetDAppByName(dapp_name);
}

rvm::ContractVersionId SimuShard::GetContractByName(const rvm::ConstString* dapp_contract_name) const
{
	return _pGlobalShard->_GetContractByName(dapp_contract_name);
}

rvm::BuildNum SimuShard::GetContractEffectiveBuild(rvm::ContractId contract) const
{
	return _pGlobalShard->GetContractEffectiveBuild(contract);
}

const rvm::DeployedContract* SimuShard::GetContractDeployed(rvm::ContractVersionId contract) const
{
	return _pGlobalShard->_GetContractDeployed(contract);
}

rvm::TokenMinterFlag SimuShard::GetTokenMinterState(rvm::TokenId tid, rvm::ContractId contract) const
{
	return _pGlobalShard->_GetTokenMinterState(tid, contract);
}

const rvm::HashValue* SimuShard::GetTxnHash(rvm::HashValue* hash_out) const
{
	return &_pTxn->Hash;
}

uint32_t SimuShard::GetMicroTxnIndex() const
{
	return 0;
}

rvm::ScopeKey SimuShard::GetScopeTarget() const
{
	rvm::Scope scope = _GetScope();
	if(scope == rvm::Scope::Global || scope == rvm::Scope::Shard)
		return { nullptr, 0 };

	return { (uint8_t*)&_pTxn->Target.u256, _pTxn->Target.target_size };
}

rvm::ConstAddress* SimuShard::GetInitiator() const 
{
	ASSERT(_pTxn);
	if(_pTxn->IsRelay())return &_pTxn->Initiator;
	if(_pTxn->Target.target_size == sizeof(rvm::Address))return &_pTxn->Target.addr;

	return nullptr;
}

rvm::ConstStateData SimuShard::GetState(rvm::ContractScopeId contract, const rvm::ScopeKey* key) const
{
	auto scope = rvm::CONTRACT_SCOPE(contract);
	auto scope_type = rvm::SCOPE_TYPE(scope);

	ASSERT(scope_type != rvm::ScopeType::Contract);
	ShardStateKey k = { contract, *key };

	const SimuState* ret = nullptr;
	if(rvm::ScopeType::ScatteredMapOnGlobal == scope_type)
		ret = _pGlobalShard->_ShardKeyedStates.Get(k);
	else if(rvm::ScopeType::ScatteredMapOnShard == scope_type)
		ret = _ShardKeyedStates.Get(k);
	else 
		ASSERT(0);

	if(ret)return { ret->Data, ret->DataSize, ret->Version };
	else return { nullptr, 0, (rvm::BuildNum)0 };
}

rvm::ConstStateData	SimuShard::GetState(rvm::ContractScopeId cid) const
{
	const SimuState* ret = nullptr;
	auto scope = rvm::CONTRACT_SCOPE(cid);

	switch(scope)
	{
	case rvm::Scope::Shard:
		{
			if(IsGlobal())goto EMPTY_STATE;
			ret = _ShardStates.Get(cid);
		}
		break;
	case rvm::Scope::Global:
		{
			ret = IsGlobal()?_ShardStates.Get(cid):
							 _pGlobalShard->_ShardStates.Get(cid);
		}
		break;
	default:
		if(IsGlobal())goto EMPTY_STATE;
		if(_GetScope() == scope)
		{
			SimuAddressContract k{ _pTxn->Target, cid };
			ret = _AddressStates.Get(k);
		}
	}

	if(ret)
		return { ret->Data, ret->DataSize, ret->Version };

EMPTY_STATE:
	return { nullptr, 0, (rvm::BuildNum)0 };
}

uint8_t* SimuShard::AllocateStateMemory(uint32_t dataSize)
{
#ifndef __APPLE__
	auto* s = SimuState::Create(dataSize, rvm::BuildNum(0), _ShardIndex);
#else
	auto* s = SimuState::Create(dataSize, rvm::BuildNum(0));
#endif
	return s?s->Data:nullptr;
}

void SimuShard::DiscardStateMemory(uint8_t* state)
{
	reinterpret_cast<SimuState*>(state - offsetof(SimuState, Data))->Release();
}

void SimuShard::CommitNewState(rvm::ContractInvokeId contract, const rvm::ScopeKey* key, uint8_t* state)
{
	auto scope = rvm::CONTRACT_SCOPE(contract);
	auto scope_type = rvm::SCOPE_TYPE(scope);

	ASSERT(scope_type != rvm::ScopeType::Contract);

	auto* s = (SimuState*)(state - offsetof(SimuState, Data));
	s->Version = rvm::CONTRACT_BUILD(contract);
	ShardStateKey k = { rvm::CONTRACT_UNSET_BUILD(contract), *key };

	_ShardKeyedStates.Set(k, s);
}

void SimuShard::CommitNewState(rvm::ContractInvokeId ciid, uint8_t* state)
{
	auto* s = (SimuState*)(state - offsetof(SimuState, Data));
	s->Version = rvm::CONTRACT_BUILD(ciid);

	auto csid = rvm::CONTRACT_UNSET_BUILD(ciid);
	auto scope = rvm::CONTRACT_SCOPE(ciid);
	switch (scope)
	{
	case rvm::Scope::Shard:
		ASSERT(!_IsGlobalScope());
		ASSERT(!IsGlobal());
		_ShardStates.Set(csid, s);
		break;
	case rvm::Scope::Global:
		ASSERT(_IsGlobalScope());
		ASSERT(IsGlobal());
		_ShardStates.Set(csid, s);
		break;
	default:
		{
			ASSERT(!_IsGlobalScope());
			ASSERT(_GetScope() == scope);

			SimuAddressContract k = { _pTxn->Target, csid };
			_AddressStates.Set(k, s);
		}
	}
}

rvm::ConstAddress* SimuShard::GetBlockCreator() const
{
	return &_pSimulator->GetMiner();
}

SimuTxn* SimuShard::_CreateRelayTxn(rvm::ContractInvokeId ciid, rvm::OpCode opcode, const rvm::ConstData* args_serialized, uint32_t gas_redistribution_weight) const
{
	auto scope = rvm::CONTRACT_SCOPE(ciid);
	if(rvm::CONTRACT_ENGINE(ciid) != rvm::EngineId::SOLIDITY_EVM && scope == rvm::ScopeInvalid) return nullptr;
#ifndef __APPLE__
	auto* txn = SimuTxn::CreateRelay(const_cast<std::pmr::unsynchronized_pool_resource*>(&_MemoryPool), args_serialized?(uint32_t)args_serialized->DataSize:0U, 0);
#else
	auto* txn = SimuTxn::Create(args_serialized?(uint32_t)args_serialized->DataSize:0U, 0);
#endif
	txn->Type = rvm::InvokeContextType::RelayInbound;
	txn->Contract = ciid;
	txn->Op = opcode;
	txn->Timestamp = _GetBlockTime();
	txn->Flag = TXN_RELAY;
	txn->OriginateHeight = _BlockHeight;
	ASSERT(_ShardIndex <= 65535)
	txn->OriginateShardIndex = uint16_t(_ShardIndex);
	txn->OriginateShardOrder = _ShardOrder;
	txn->Gas = 0;
	txn->GasRedistributionWeight = gas_redistribution_weight;

#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	// Carry source identity with the relay itself. Trace builds obtain the
	// ordinal from the marker stack; optimization-only builds receive the same
	// ordinal through SetRelayOriginMetadata emitted by the compiler.
	const bool hasTraceMarker =
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
		!_RelayTraceMarkerStack.empty();
#else
		false;
#endif
	if(_RelayOriginMetadataPending || hasTraceMarker)
	{
		uint32_t siteOrdinal = _RelayOriginSiteOrdinal;
		std::string moduleIdentity;
		std::string sourceFunctionId;
		std::string siteId;
		const auto* sourceDeployed = _pTxn == nullptr
			? nullptr
			: GetContractDeployed(
				rvm::CONTRACT_UNSET_SCOPE(_pTxn->Contract));
		if(sourceDeployed != nullptr)
		{
			moduleIdentity = relay_plan::RelayPlanLoader::RuntimeHashIdentity(
				sourceDeployed->Module);
			const auto sourcePlan = _pSimulator->LookupRelayPlan(
				moduleIdentity,
				_pTxn == nullptr ? 0U : static_cast<uint32_t>(_pTxn->Op));
			if(sourcePlan.function != nullptr)
				sourceFunctionId = sourcePlan.function->sourceFunctionId;
			if(sourcePlan.loadResult && sourcePlan.loadResult->manifest)
			{
				const auto site =
					sourcePlan.loadResult->manifest->sitesByOrdinal.find(
						siteOrdinal);
				if(site != sourcePlan.loadResult->manifest->sitesByOrdinal.end())
					siteId = site->second.id;
			}
		}
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
		if(hasTraceMarker)
		{
			const auto& marker = _RelayTraceMarkerStack.back();
			moduleIdentity = GetRelayTraceModuleIdentity(marker.module);
			siteOrdinal = marker.ordinal;
			relay_trace::ManifestRelaySite site;
			if(_pSimulator->ResolveRelayTraceSite(
				marker.module,
				marker.ordinal,
				site))
				siteId = site.id;
			_pSimulator->ResolveRelayTraceFunction(
				sourceDeployed == nullptr
					? marker.module
					: sourceDeployed->Module,
				_pTxn == nullptr ? 0U : static_cast<uint32_t>(_pTxn->Op),
				sourceFunctionId);
		}
#endif
		PopulateRelayOriginMetadata(
			txn,
			moduleIdentity,
			sourceFunctionId,
			_pTxn == nullptr ? 0U : static_cast<uint32_t>(_pTxn->Op),
			siteOrdinal,
			siteId);
		_RelayOriginMetadataPending = false;
	}
#endif

	ASSERT(txn->ArgsSerializedSize == args_serialized->DataSize);
	memcpy(txn->SerializedData, args_serialized->DataPtr, args_serialized->DataSize);

	sec::Hash<sec::HASH_SHA256>().Calculate(((char*)txn) + sizeof(rvm::HashValue), txn->GetSize() - sizeof(rvm::HashValue), &txn->Hash);

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	auto* relayTraceCollector = _pSimulator->GetRelayTraceCollector();
	RunRelayTraceObservation(
		relayTraceCollector,
		"relay creation",
		[&]()
	{
		if (relayTraceCollector == nullptr)
			return;

		std::string targetModuleId;
		const auto* deployed = GetContractDeployed(
			rvm::CONTRACT_UNSET_SCOPE(ciid));
		if (deployed != nullptr)
			targetModuleId =
				GetRelayTraceModuleIdentity(deployed->Module);

		const rvm::ConstData copiedArguments = txn->GetArguments();
		relay_trace::RelayCreateInput input;
		input.parentTransaction = _pTxn;
		input.childTransaction = txn;
		input.targetContractInvoke = ciid;
		input.targetOpcode = static_cast<uint32_t>(opcode);
		input.serializedArguments =
			static_cast<const uint8_t*>(copiedArguments.DataPtr);
		input.serializedArgumentsSize = copiedArguments.DataSize;
		input.targetModuleId = std::move(targetModuleId);
		input.ownerShard = _ShardIndex;
		relayTraceCollector->RegisterRelayCreation(input);
	});
#endif

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	if(auto* audit = _pSimulator->GetRelayOptimizationAudit())
		audit->InheritTransaction(_pTxn, txn);
#endif

	return txn;
}

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
void SimuShard::_FinalizeRelayTraceEmission(
	SimuTxn* txn,
	relay_trace::RelayKind relayKind,
	relay_trace::ScopeKind targetScopeKind) const noexcept
{
	auto* collector = _pSimulator->GetRelayTraceCollector();
	RunRelayTraceObservation(
		collector,
		"relay emission finalization",
		[&]()
	{
		if (collector == nullptr || txn == nullptr)
			return;

		std::string relaySiteId;
		const auto pending = collector->PendingRelayIdentity(txn);
		if (pending &&
			pending->relaySiteOrdinal !=
				relay_trace::InvalidRelaySiteOrdinal &&
			!_RelayTraceMarkerStack.empty())
		{
			relay_trace::ManifestRelaySite site;
			if (_pSimulator->ResolveRelayTraceSite(
				_RelayTraceMarkerStack.back().module,
				pending->relaySiteOrdinal,
				site))
			{
				relaySiteId = site.id;
				collector->SetPendingRelayResolvedIdentity(
					txn,
					site.id,
					site.sourceFunctionId);
			}
		}

		relay_trace::RelayFinalizeInput input;
		input.childTransaction = txn;
		if (relayKind == relay_trace::RelayKind::CustomScope ||
			relayKind == relay_trace::RelayKind::DeferredNext)
		{
			input.targetData =
				reinterpret_cast<const uint8_t*>(&txn->Target.u512);
			input.targetSize = txn->Target.target_size;
		}
		input.targetScope = targetScopeKind;
		input.relayKind = relayKind;
		input.relaySiteId = std::move(relaySiteId);
		collector->FinalizeRelayEmission(input);
	});
}

void SimuShard::PushRelayTraceSite(
	const rvm::ContractModuleID& emittingModule,
	uint32_t siteOrdinal) noexcept
{
	auto* collector = _pSimulator == nullptr
		? nullptr
		: _pSimulator->GetRelayTraceCollector();
	try
	{
		if (collector == nullptr ||
			collector->Mode() == relay_trace::TraceMode::Off)
		{
			return;
		}

		const uint64_t generation = collector->PushMarker(
			_pTxn,
			GetRelayTraceModuleIdentity(emittingModule),
			siteOrdinal);
		if (generation != 0)
		{
			_RelayTraceMarkerStack.push_back(
				{emittingModule, siteOrdinal, generation});
		}
	}
	catch (...)
	{
		// This trace-only ABI is noexcept. A failed observation must never
		// alter contract exception propagation or worker execution.
		if (collector != nullptr)
			collector->RecordObserverFailureNoexcept(
				"relay-site marker push");
	}
}

void SimuShard::PopRelayTraceSite(
	const rvm::ContractModuleID& emittingModule,
	uint32_t siteOrdinal) noexcept
{
	auto* collector = _pSimulator == nullptr
		? nullptr
		: _pSimulator->GetRelayTraceCollector();
	try
	{
		if (collector == nullptr ||
			collector->Mode() == relay_trace::TraceMode::Off)
		{
			return;
		}

		uint64_t generation = 0;
		if (!_RelayTraceMarkerStack.empty())
		{
			generation = _RelayTraceMarkerStack.back().generation;
			_RelayTraceMarkerStack.pop_back();
		}
		collector->PopMarker(
			_pTxn,
			generation,
			GetRelayTraceModuleIdentity(emittingModule),
			siteOrdinal,
			true);
	}
	catch (...)
	{
		// See PushRelayTraceSite(): trace errors are latched by the collector
		// when representable and are surfaced only at the simulator safe point.
		if (collector != nullptr)
			collector->RecordObserverFailureNoexcept(
				"relay-site marker pop");
	}
}
#endif

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
void SimuShard::_AppendRelayEmission(
	SimuTxn* txn,
	uint64_t generationBeginNs)
{
	auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
	const size_t capacityBefore = _RelayEmitted.reserve_size();
	_RelayEmitted.push_back(txn);
	const size_t capacityAfter = _RelayEmitted.reserve_size();
	metrics->logicalRelayEmissions.fetch_add(
		1,
		std::memory_order_relaxed);
	if(capacityAfter > capacityBefore)
	{
		metrics->relayBufferCapacityGrowthEvents.fetch_add(
			1,
			std::memory_order_relaxed);
	}
	metrics->relayGenerationTimeNs.fetch_add(
		RelayOptimizationNowNs() - generationBeginNs,
		std::memory_order_relaxed);
}

namespace
{

relay_optimization::RelaySchedulerTask MakeRelaySchedulerTask(
	const SimuTxn* txn,
	uint32_t destinationShard)
{
	relay_optimization::RelaySchedulerTask task;
	if(txn == nullptr)
		return task;

	task.originateHeight = txn->OriginateHeight;
	task.originShard = txn->OriginateShardIndex;
	task.destinationShard = destinationShard;
	task.sourceOrder = txn->OriginateShardOrder;
	task.estimatedWork = 1;
	task.remainingDepth = 1;
	task.sourceOpcode = txn->RelayOriginOpcode;
	task.relaySiteOrdinal = txn->RelayOriginSiteOrdinal;
	task.globalRelay = txn->GetScope() == rvm::Scope::Global;
	task.broadcastRelay = txn->IsBroadcast();
	const bool metadataSizesValid =
		txn->RelayOriginMetadataValid != 0 &&
		txn->RelayOriginModuleSize > 0 &&
		txn->RelayOriginModuleSize <
			SimuTxn::RelayOriginModuleCapacity &&
		txn->RelayOriginFunctionSize > 0 &&
		txn->RelayOriginFunctionSize <
			SimuTxn::RelayOriginFunctionCapacity &&
		txn->RelayOriginSiteSize > 0 &&
		txn->RelayOriginSiteSize < SimuTxn::RelayOriginSiteCapacity;
	if(metadataSizesValid)
	{
		task.moduleId.assign(
			txn->RelayOriginModule,
			txn->RelayOriginModuleSize);
		task.functionId.assign(
			txn->RelayOriginFunction,
			txn->RelayOriginFunctionSize);
		task.relaySiteId.assign(
			txn->RelayOriginSite,
			txn->RelayOriginSiteSize);
	}
	// A non-trace build has no source identity, so target-key heuristics alone
	// remain insufficient for a certificate-backed reorder.
	task.opaqueRelay = !metadataSizesValid;
	task.targetKnown = !task.globalRelay && !task.broadcastRelay &&
		txn->Target.target_size != 0;
	if(task.targetKnown)
	{
		task.targetScopeKey.push_back(
			static_cast<char>(txn->Target.target_size));
		task.targetScopeKey.append(
			reinterpret_cast<const char*>(&txn->Target.u512),
			txn->Target.target_size);
	}
	task.stableId = std::to_string(task.originateHeight) + ":" +
		std::to_string(task.originShard) + ":" +
		std::to_string(task.sourceOrder) + ":" +
		std::to_string(static_cast<uint32_t>(txn->Op)) + ":" +
		task.targetScopeKey;
	return task;
}

} // namespace

SimuTxn* SimuShard::_PopRelayTxn()
{
	auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
	const auto schedulerMode =
		_pSimulator->GetRelayOptimizationConfig().schedulerMode;
	if(schedulerMode == relay_optimization::RelaySchedulerMode::FIFO)
	{
		SimuTxn* result = _PendingRelayTxns.Pop();
		if(result != nullptr)
		{
			metrics->schedulerSelectionCalls.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		return result;
	}

	const relay_optimization::RelayScheduler scheduler(schedulerMode);
	relay_optimization::RelaySchedulerDecision decision;
	const uint64_t decisionBeginNs = RelayOptimizationNowNs();
	SimuTxn* result = _PendingRelayTxns.PopSelected(
		[&](const std::vector<SimuTxn*>& ready)
		{
			std::vector<relay_optimization::RelaySchedulerTask> tasks;
			tasks.reserve(ready.size());
			for(const SimuTxn* txn : ready)
				tasks.push_back(
					MakeRelaySchedulerTask(txn, _ShardIndex));

			relay_optimization::RelaySchedulerCertificateView view;
			// A certificate view is valid only for one source module/function/
			// opcode. Mixed prefixes stay on the conservative FIFO path.
			if(schedulerMode ==
				relay_optimization::RelaySchedulerMode::
					CertificateGuidedPriority &&
				!tasks.empty() && !tasks.front().moduleId.empty() &&
				!tasks.front().functionId.empty())
			{
				const auto& first = tasks.front();
				bool sameSource = true;
				for(const auto& task : tasks)
				{
					if(task.moduleId != first.moduleId ||
						task.functionId != first.functionId ||
						task.sourceOpcode != first.sourceOpcode)
					{
						sameSource = false;
						break;
					}
				}
				if(sameSource)
				{
					auto lookup = _pSimulator->LookupRelayPlan(
						first.moduleId,
						first.sourceOpcode);
					if(lookup.loadResult && lookup.loadResult->manifest &&
						lookup.function != nullptr)
					{
						const auto& manifest =
							*lookup.loadResult->manifest;
						for(auto& task : tasks)
						{
							if(task.relaySiteId.empty())
							{
								auto site = manifest.sitesByOrdinal.find(
									task.relaySiteOrdinal);
								if(site != manifest.sitesByOrdinal.end())
									task.relaySiteId = site->second.id;
							}
						}
						auto certificate =
							manifest.parallelCertificatesByFunction.find(
								first.functionId);
						if(certificate !=
							manifest.parallelCertificatesByFunction.end())
						{
							view.function = &certificate->second;
							view.bindingTrusted =
								manifest.bindingTrusted &&
								lookup.function->bindingTrusted;
							view.functionOpcodeTrusted =
								lookup.function->functionOpcodeTrusted;
							view.optimizationEligible =
								lookup.function->optimizationEligible;
						}
					}
				}
			}
			decision = scheduler.Choose(tasks, view);
			return decision.selectedIndex;
		});
	metrics->schedulerDecisionTimeNs.fetch_add(
		RelayOptimizationNowNs() - decisionBeginNs,
		std::memory_order_relaxed);
	if(result != nullptr)
	{
		metrics->schedulerSelectionCalls.fetch_add(
			1,
			std::memory_order_relaxed);
		if(decision.reordered)
		{
			metrics->schedulerReorders.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		if(decision.certificateUsed)
		{
			metrics->schedulerCertificateUses.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		if(decision.reason !=
			relay_optimization::RelaySchedulerDecisionReason::Priority)
		{
			metrics->schedulerFallbacks.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		else
		{
			metrics->schedulerPrioritySelections.fetch_add(
				1,
				std::memory_order_relaxed);
		}
	}
	return result;
}
#endif
void SimuShard::SetRelayOriginMetadata(uint32_t siteOrdinal)
{
	_RelayOriginSiteOrdinal = siteOrdinal;
	_RelayOriginMetadataPending = true;
}

bool SimuShard::EmitRelayToScope(rvm::ContractInvokeId ciid, const rvm::ScopeKey* key, rvm::OpCode opcode, const rvm::ConstData* args_serialized, uint32_t gas_redistribution_weight)
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint64_t relayGenerationBeginNs =
		RelayOptimizationNowNs();
#endif
	auto* txn = _CreateRelayTxn(ciid, opcode, args_serialized, gas_redistribution_weight);
	ASSERT(txn->Contract == ciid);
	rvm::ScopeKeySized kst = rvm::SCOPE_KEYSIZETYPE(rvm::CONTRACT_SCOPE(ciid));

	switch(kst)
	{
	case rvm::ScopeKeySized::Address:
		ASSERT(key->Size == sizeof(rvm::Address));
		txn->Target = ScopeTarget(*(rvm::Address*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt32:
		ASSERT(key->Size == sizeof(uint32_t));
		txn->Target = ScopeTarget(*(uint32_t*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt64:
		ASSERT(key->Size == sizeof(uint64_t));
		txn->Target = ScopeTarget(*(uint64_t*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt96:
		ASSERT(key->Size == sizeof(rvm::UInt96));
		txn->Target = ScopeTarget(*(rvm::UInt96*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt128:
		ASSERT(key->Size == sizeof(rvm::UInt128));
		txn->Target = ScopeTarget(*(rvm::UInt128*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt160:
		ASSERT(key->Size == sizeof(rvm::UInt160));
		txn->Target = ScopeTarget(*(rvm::UInt160*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt256:
		ASSERT(key->Size == sizeof(rvm::UInt256));
		txn->Target = ScopeTarget(*(rvm::UInt256*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt336:
		ASSERT(key->Size == sizeof(rvm::UInt336));
		txn->Target = ScopeTarget(*(rvm::UInt336*)key->Data);
		break;
	case rvm::ScopeKeySized::UInt512:
		ASSERT(key->Size == sizeof(rvm::UInt512));
		txn->Target = ScopeTarget(*(rvm::UInt512*)key->Data);
		break;
	default:
		break;
	}
	txn->Initiator = _pTxn->IsRelay()?_pTxn->Initiator:_pTxn->Target.addr;

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	_FinalizeRelayTraceEmission(
		txn,
		relay_trace::RelayKind::CustomScope,
		GetRelayTraceScopeKind(rvm::CONTRACT_SCOPE(ciid)));
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	_AppendRelayEmission(txn, relayGenerationBeginNs);
#else
	_RelayEmitted.push_back(txn);
#endif
	return true;
}

bool SimuShard::EmitRelayToGlobal(rvm::ContractInvokeId cid, rvm::OpCode opcode, const rvm::ConstData* args_serialized, uint32_t gas_redistribution_weight)
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint64_t relayGenerationBeginNs =
		RelayOptimizationNowNs();
#endif
	auto* txn = _CreateRelayTxn(cid, opcode, args_serialized, gas_redistribution_weight);

	rt::Zero(txn->Target);
	txn->Initiator = _pTxn->IsRelay()?_pTxn->Initiator:_pTxn->Target.addr;

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	_FinalizeRelayTraceEmission(
		txn,
		relay_trace::RelayKind::Global,
		relay_trace::ScopeKind::Global);
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	_AppendRelayEmission(txn, relayGenerationBeginNs);
#else
	_RelayEmitted.push_back(txn);
#endif
	return true;
}

bool SimuShard::EmitRelayDeferred(rvm::ContractInvokeId cid, rvm::OpCode opcode, const rvm::ConstData* args_serialized, uint32_t gas_redistribution_weight)
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint64_t relayGenerationBeginNs =
		RelayOptimizationNowNs();
#endif
	ASSERT(_pTxn->GetScope() != rvm::Scope::Shard);

	auto* txn = _CreateRelayTxn(cid, opcode, args_serialized, gas_redistribution_weight);
	txn->Flag = TXN_DEFERRED;
	txn->Target = _pTxn->Target;
	txn->Initiator = _pTxn->IsRelay()?_pTxn->Initiator:_pTxn->Target.addr;

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	_FinalizeRelayTraceEmission(
		txn,
		relay_trace::RelayKind::DeferredNext,
		GetRelayTraceScopeKind(rvm::CONTRACT_SCOPE(cid)));
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	_AppendRelayEmission(txn, relayGenerationBeginNs);
#else
	_RelayEmitted.push_back(txn);
#endif
	return true;
}

bool SimuShard::EmitBroadcastToShards(rvm::ContractInvokeId cid, rvm::OpCode opcode, const rvm::ConstData* args_serialized, uint32_t gas_redistribution_weight)
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint64_t relayGenerationBeginNs =
		RelayOptimizationNowNs();
#endif
	auto* txn = _CreateRelayTxn(cid, opcode, args_serialized, gas_redistribution_weight);

	txn->Flag = (SimuTxnFlag)(txn->Flag|TXN_BROADCAST);
	rt::Zero(txn->Target);
	txn->Initiator = _pTxn->IsRelay()?_pTxn->Initiator:_pTxn->Target.addr;
	if(rvm::CONTRACT_SCOPE(_pTxn->Contract) == rvm::Scope::Global)
	{
		txn->Type = rvm::InvokeContextType::Scheduled; // Global to Shards
	}

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	_FinalizeRelayTraceEmission(
		txn,
		relay_trace::RelayKind::AllShards,
		relay_trace::ScopeKind::Shard);
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	_AppendRelayEmission(txn, relayGenerationBeginNs);
#else
	_RelayEmitted.push_back(txn);
#endif
	return true;
}

rvm::ExecuteResult SimuShard::Invoke(uint32_t gas_limit, rvm::ContractInvokeId contract, rvm::OpCode opcode, const rvm::ConstData* args_serialized)
{
	rvm::ExecuteResult ret;
	rt::Zero(ret);
	ret.Code = rvm::InvokeErrorCode::ContractUnavailable;

	return ret;
}

void SimuShard::SetReturnValue(const rvm::ConstData* args_serialized)
{
	auto* d = SetReturnValueClaim((uint32_t)args_serialized->DataSize);
	ASSERT(d);

	memcpy(d, args_serialized->DataPtr, (uint32_t)args_serialized->DataSize);
	SetReturnValueFinalize((uint32_t)args_serialized->DataSize);
}

bool SimuShard::NativeTokensDeposit(const rvm::ConstNativeToken* tokens, uint32_t count)
{
	ASSERT(_pTxn);
	if (_pTxn->Target.target_size != sizeof(rvm::Address))
		return false;

	for (uint32_t i = 0; i < count; i++)
	{
		if (tokens[i].Amount.Sign)
			return false;
	}

	ASSERT(_pTxn->Target.target_size == sizeof(rvm::Address));

	CoreContractState<CORE_WALLET> s = _AddressStates.Get({ _pTxn->Target.addr, -CORE_WALLET });
	rvm::CoinsWalletMutable next = s.DeriveMutable();

	for (uint32_t i = 0; i < count; i++)
		next.Deposit(tokens[i]);

	CommitCoreState<CORE_WALLET>(next);

	return true;
}

void SimuShard::NativeTokensSupplyChange(const rvm::ConstNativeToken* tokens, uint32_t count)
{
	thread_local rt::String str;

	str.Empty();
	for (uint32_t i = 0; i < count; i++)
	{
		ext::BigNumRef bRef = ext::BigNumRef(tokens[i].Amount.Sign, tokens[i].Amount.Blocks, tokens[i].Amount.BlockCount);
		ext::_details::BN_ToString(bRef, str, 10);
		str += ' ';
		str += rvm::TokenIdToSymbol(tokens[i].Token);
		if (i < count - 1)
		{
			str += ',';
			str += ' ';
		}
	}
	_LOGC_VERBOSE("[PRD]: Token Supply Change: " << str);
}

void SimuShard::Init()
{
	rt::Zero(_BlockHash);
	rt::Zero(_PrevBlockHash);
	_BlockTimeBase = os::Timestamp::Get();
	_BlockHeight = 0;
	_ConfirmedTxnCount = 0;

	_pSimulator->CreateExecuteUnit(&_ExecUnits);

	_GoNextBlock.Reset();
	_BlockCreator.Create([this](){ _BlockCreationRoutine(); });
}

void SimuShard::Term()
{
	if(_BlockCreator.IsRunning())
	{
		_BlockCreator.WantExit() = true;
		_GoNextBlock.Set();
		_BlockCreator.WaitForEnding();
	}

	_AddressStates.Empty();
	_ShardStates.Empty();
	_ShardKeyedStates.Empty();

	for (auto b : _Chain)
		b->Release();

	_Chain.ShrinkSize(0);
}

void SimuShard::SetState(rvm::ContractScopeId csid, SimuState* s)
{
#if defined(PLATFORM_DEBUG_BUILD)
	auto scope = rvm::CONTRACT_SCOPE(csid);
	ASSERT(	(scope == rvm::Scope::Global && IsGlobal()) ||
			(scope == rvm::Scope::Shard && !IsGlobal())
	);
#endif

	_ShardStates.Set(csid, s);
}

void SimuShard::SetState(rvm::ContractScopeId csid, const rvm::Address& target, SimuState* s)
{
	ASSERT(rvm::CONTRACT_SCOPE(csid) == rvm::Scope::Address);
	ASSERT(_pSimulator->GetShardIndex(target) == _ShardIndex);

	_AddressStates.Set({ ScopeTarget(target), csid }, s);
}

void SimuShard::PushNormalTxn(SimuTxn* t)
{
	ASSERT(!t->IsRelay());

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
	const uint64_t queueBeginNs = RelayOptimizationNowNs();
#endif
	bool first_item = _PendingTxns.Push(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	metrics->queueSinglePushCalls.fetch_add(
		1,
		std::memory_order_relaxed);
	metrics->queueLockAcquisitions.fetch_add(
		1,
		std::memory_order_relaxed);
	metrics->queuePushTimeNs.fetch_add(
		RelayOptimizationNowNs() - queueBeginNs,
		std::memory_order_relaxed);
#endif
	_pSimulator->OnTxnPushed();

	if(first_item && _pSimulator->IsShardingAsync() && !_pSimulator->IsChainPaused() && !_pSimulator->IsChainStepping())
	{
		_GoNextBlock.Set();
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
		metrics->queueNotifications.fetch_add(
			1,
			std::memory_order_relaxed);
#endif
	}
}

void SimuShard::PushIntraRelay(SimuTxn* t)
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
	const uint64_t queueBeginNs = RelayOptimizationNowNs();
	auto* audit = _pSimulator->GetRelayOptimizationAudit();
	const auto auditRoot = audit
		? audit->RootForTransaction(t)
		: std::optional<uint64_t>{};
	const size_t auditSizeBefore = auditRoot
		? _IntraRelayTxns.GetSize()
		: 0;
#endif
	_IntraRelayTxns.Push(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	if(auditRoot)
	{
		const size_t auditSizeAfter =
			_IntraRelayTxns.GetSize();
		const uint64_t observed =
			auditSizeAfter >= auditSizeBefore
				? static_cast<uint64_t>(
					auditSizeAfter - auditSizeBefore)
				: std::numeric_limits<uint64_t>::max();
		audit->CheckClassifiedExactlyOnce(
			*auditRoot,
			observed);
		audit->CheckDestinationShard(
			*auditRoot,
			_pSimulator->GetShardIndex(t->Target),
			observed == 1
				? _ShardIndex
				: std::numeric_limits<uint32_t>::max());
	}
	metrics->queueSinglePushCalls.fetch_add(
		1,
		std::memory_order_relaxed);
	metrics->queueLockAcquisitions.fetch_add(
		1,
		std::memory_order_relaxed);
	metrics->queuePushTimeNs.fetch_add(
		RelayOptimizationNowNs() - queueBeginNs,
		std::memory_order_relaxed);
#endif
}

void SimuShard::PushRelayTxn(SimuTxn** txns, uint32_t count)
{
	if(count)
	{
		ASSERT(txns[0]->IsRelay());

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
		auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
		auto* audit =
			_pSimulator->GetRelayOptimizationAudit();
		PendingBatchAuditObservation auditObservation;
		std::vector<uint64_t> auditBatchRoots;
		const bool usesBatchFastPath =
			_pSimulator->GetRelayOptimizationConfig().
				UsesBatchFastPath();
		if(audit && usesBatchFastPath)
		{
			audit->SnapshotUniqueTransactionRoots(
				txns,
				count,
				auditBatchRoots);
		}
		bool auditBatchCommitted = false;
		const uint64_t queueBeginNs = RelayOptimizationNowNs();
		bool first_item = false;
		if(usesBatchFastPath)
		{
			metrics->queueBatchPushCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			metrics->queueLockAcquisitions.fetch_add(
				1,
				std::memory_order_relaxed);
			const PendingPushResult pushed =
				_PendingRelayTxns.PushBatch(
					txns,
					count,
					audit ? &auditObservation : nullptr);
			if(pushed.committed)
			{
				first_item = pushed.wasEmpty;
				metrics->queueBatchElements.fetch_add(
					pushed.inserted,
					std::memory_order_relaxed);
				metrics->ObserveBatchSize(pushed.inserted);
				auditBatchCommitted = audit != nullptr;
			}
			else
			{
				// The transactional fast path leaves the target queue and
				// caller ownership unchanged. Fall back to the exact legacy
				// bulk path without dropping or duplicating a pointer.
				metrics->queueBatchFallbacks.fetch_add(
					1,
					std::memory_order_relaxed);
				metrics->queueLegacyBulkPushCalls.fetch_add(
					1,
					std::memory_order_relaxed);
				metrics->queueLockAcquisitions.fetch_add(
					1,
					std::memory_order_relaxed);
				first_item =
					_PendingRelayTxns.Push(txns, count);
			}
		}
		else
		{
			metrics->queueLegacyBulkPushCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			metrics->queueLockAcquisitions.fetch_add(
				1,
				std::memory_order_relaxed);
			first_item = _PendingRelayTxns.Push(txns, count);
		}
		metrics->queuePushTimeNs.fetch_add(
			RelayOptimizationNowNs() - queueBeginNs,
			std::memory_order_relaxed);
#else
		bool first_item = _PendingRelayTxns.Push(txns, count);
#endif
		//_pSimulator->OnTxnPushed(count);
		_pSimulator->OnTxnPushed(count);

		if(first_item && _pSimulator->IsShardingAsync() && !_pSimulator->IsChainPaused() && !_pSimulator->IsChainStepping())
		{
			_GoNextBlock.Set();
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			metrics->queueNotifications.fetch_add(
				1,
				std::memory_order_relaxed);
#endif
		}
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
		// Observe a committed optimized batch only after pending-work
		// accounting and wakeup are complete. Audit diagnostics are
		// allocation-free/noexcept here, so they cannot split queue ownership
		// from simulator accounting.
		if(auditBatchCommitted)
		{
			const uint64_t queueSizeDelta =
				auditObservation.sizeAfter >=
					auditObservation.sizeBefore
					? static_cast<uint64_t>(
						auditObservation.sizeAfter -
						auditObservation.sizeBefore)
					: std::numeric_limits<uint64_t>::max();
			const uint64_t observedTailCount =
				static_cast<uint64_t>(
					auditObservation.inserted.size());
			bool identityAndOrderPreserved =
				auditObservation.inserted.size() == count;
			if(identityAndOrderPreserved)
			{
				for(uint32_t index = 0; index < count; ++index)
				{
					if(auditObservation.inserted[index] !=
						txns[index])
					{
						identityAndOrderPreserved = false;
						break;
					}
				}
			}
			for(uint64_t root : auditBatchRoots)
			{
				audit->CheckBatchObservation(
					root,
					count,
					queueSizeDelta,
					observedTailCount,
					identityAndOrderPreserved);
			}
		}
#endif
	}
}

RelayEmission::RelayEmission(ChainSimulator* s, SimuShard* shard)
	:_pSimulator(s),_pShard(shard)
{
	_ToShards.SetSize(s->GetShardCount());
}

RelayEmission::~RelayEmission()
{
	auto rel = [](rt::BufferEx<SimuTxn*>& txns)
	{
		for(auto t : txns)t->Release();
		txns.SetSize(0);
	};

	rel(_ToGlobal);
	for(auto& s : _ToShards)rel(s);

	_ToShards.SetSize(0);
}

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
void RelayEmission::ReserveForPlan(
	const relay_plan::FunctionRelayPlan& plan,
	uint64_t logicalRelayBound,
	uint64_t maximumReserve)
{
	if(logicalRelayBound == 0)
		return;

	auto* metrics = _pSimulator->GetRelayOptimizationMetrics();

	if(plan.mayGlobal)
	{
		size_t required = 0;
		if(relay_optimization::CheckedRequiredCapacity(
			_ToGlobal.GetSize(),
			logicalRelayBound,
			maximumReserve,
			required))
		{
			if(!_ToGlobal.reserve(required))
			{
				metrics->relayBufferReserveFailures.fetch_add(
					1,
					std::memory_order_relaxed);
			}
		}
	}

	if(plan.mayBroadcast)
	{
		uint64_t aggregate = 0;
		if(relay_optimization::CheckedAggregateBroadcastReserve(
			logicalRelayBound,
			static_cast<uint32_t>(_ToShards.GetSize()),
			maximumReserve,
			aggregate))
		{
			bool allReserved = true;
			for(auto& shardBuffer : _ToShards)
			{
				size_t required = 0;
				if(!relay_optimization::CheckedRequiredCapacity(
					shardBuffer.GetSize(),
					logicalRelayBound,
					maximumReserve,
					required) ||
					!shardBuffer.reserve(required))
				{
					allReserved = false;
					break;
				}
			}
			if(allReserved)
			{
				metrics->broadcastCloneReserveCalls.fetch_add(
					1,
					std::memory_order_relaxed);
			}
			else
			{
				metrics->relayBufferReserveFailures.fetch_add(
					1,
					std::memory_order_relaxed);
			}
		}
	}

}
#endif

uint32_t RelayEmission::Collect(SimuTxn* origin, rt::BufferEx<SimuTxn*>& txns, uint64_t remained_gas)
{
#define APPEND_TRACE(t)		_pShard->AppendToTxnTrace(origin, t)
	const uint32_t logicalRelayCount =
		static_cast<uint32_t>(txns.GetSize());
	// calculate total gas redistribution weight
	uint32_t total_weight = 0;
	for(SimuTxn* t : txns)
	{
		switch(t->GetScope())
		{
		case rvm::Scope::Shard:
			total_weight += t->GasRedistributionWeight * (uint32_t)_ToShards.GetSize();
			break;
		default:
			total_weight += t->GasRedistributionWeight;
		}
	}
	uint64_t relay_gas_unit = remained_gas / total_weight;
	remained_gas -= relay_gas_unit * total_weight;

	for(SimuTxn* t : txns)
	{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
		auto* metrics = _pSimulator->GetRelayOptimizationMetrics();
		auto* audit = _pSimulator->GetRelayOptimizationAudit();
		const auto auditRoot = audit
			? audit->RootForTransaction(t)
			: std::optional<uint64_t>{};
#endif
		t->Gas += t->GasRedistributionWeight * relay_gas_unit;
		if(t->IsDeferred())
		{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			const size_t routeSizeBefore = auditRoot
				? _ToNextBlock.GetSize()
				: 0;
#endif
			_ToNextBlock.push_back(t);
			APPEND_TRACE(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			metrics->physicalRelayRoutes.fetch_add(
				1,
				std::memory_order_relaxed);
			if(auditRoot)
			{
				uint64_t occurrences = 0;
				for(SimuTxn* routed : _ToNextBlock)
				{
					if(routed == t)
						++occurrences;
				}
				const size_t routeSizeAfter =
					_ToNextBlock.GetSize();
				if(routeSizeAfter < routeSizeBefore ||
					routeSizeAfter - routeSizeBefore != 1)
				{
					occurrences = routeSizeAfter >=
						routeSizeBefore
						? routeSizeAfter - routeSizeBefore
						: std::numeric_limits<uint64_t>::max();
				}
				audit->CheckClassifiedExactlyOnce(
					*auditRoot,
					occurrences);
			}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
			{
				auto* relayTraceCollector =
					_pSimulator->GetRelayTraceCollector();
				RunRelayTraceObservation(
					relayTraceCollector,
					"deferred relay route recording",
					[&]()
				{
					if (relayTraceCollector == nullptr)
						return;
					relayTraceCollector->RecordRoute(
						t,
						_pShard->GetShardIndex(),
						relay_trace::RouteKind::DeferredNext,
						_pSimulator->GetShardCount());
				});
			}
#endif
			continue;
		}

		ASSERT(t->IsRelay());

		switch(t->GetScope())
		{
		case rvm::Scope::Global:
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			{
			const size_t routeSizeBefore = auditRoot
				? _ToGlobal.GetSize()
				: 0;
#endif
			_ToGlobal.push_back(t);
			APPEND_TRACE(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			metrics->physicalRelayRoutes.fetch_add(
				1,
				std::memory_order_relaxed);
			if(auditRoot)
			{
				uint64_t occurrences = 0;
				for(SimuTxn* routed : _ToGlobal)
				{
					if(routed == t)
						++occurrences;
				}
				const size_t routeSizeAfter =
					_ToGlobal.GetSize();
				if(routeSizeAfter < routeSizeBefore ||
					routeSizeAfter - routeSizeBefore != 1)
				{
					occurrences = routeSizeAfter >=
						routeSizeBefore
						? routeSizeAfter - routeSizeBefore
						: std::numeric_limits<uint64_t>::max();
				}
				audit->CheckClassifiedExactlyOnce(
					*auditRoot,
					occurrences);
				audit->CheckDestinationShard(
					*auditRoot,
					rvm::GlobalShard,
					occurrences == 1
						? rvm::GlobalShard
						: std::numeric_limits<uint32_t>::max());
			}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
			{
				auto* relayTraceCollector =
					_pSimulator->GetRelayTraceCollector();
				RunRelayTraceObservation(
					relayTraceCollector,
					"global relay route recording",
					[&]()
				{
					if (relayTraceCollector == nullptr)
						return;
					relayTraceCollector->RecordRoute(
						t,
						rvm::GlobalShard,
						relay_trace::RouteKind::Global,
						_pSimulator->GetShardCount());
				});
			}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			}
#endif
			break;
		case rvm::Scope::Shard:
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			{
			uint64_t observedPhysicalCount = 0;
			bool exactlyOnePerDestination = true;
#endif
			for(uint32_t i = 1; i < _ToShards.GetSize(); i++)
			{
				SimuTxn* _clone = t->Clone();
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
				if(audit)
					audit->InheritTransaction(t, _clone);
				const size_t routeSizeBefore = auditRoot
					? _ToShards[i].GetSize()
					: 0;
#endif
				_ToShards[i].push_back(_clone);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
				if(auditRoot)
				{
					const size_t routeSizeAfter =
						_ToShards[i].GetSize();
					const uint64_t delta =
						routeSizeAfter >= routeSizeBefore
							? static_cast<uint64_t>(
								routeSizeAfter -
									routeSizeBefore)
							: std::numeric_limits<
								uint64_t>::max();
					if(observedPhysicalCount <=
						std::numeric_limits<uint64_t>::max() -
							delta)
					{
						observedPhysicalCount += delta;
					}
					else
					{
						observedPhysicalCount =
							std::numeric_limits<uint64_t>::max();
					}
					exactlyOnePerDestination =
						exactlyOnePerDestination &&
						delta == 1 &&
						routeSizeAfter != 0 &&
						_ToShards[i][routeSizeAfter - 1] ==
							_clone;
				}
#endif
				APPEND_TRACE(_clone);
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
				{
					auto* relayTraceCollector =
						_pSimulator->GetRelayTraceCollector();
					RunRelayTraceObservation(
						relayTraceCollector,
						"broadcast relay clone recording",
						[&]()
					{
						if (relayTraceCollector == nullptr)
							return;
						relayTraceCollector->CloneRelayMetadata(
							t,
							_clone,
							i);
						relayTraceCollector->RecordRoute(
							_clone,
							i,
							relay_trace::RouteKind::
								AllShardsBroadcast,
							_pSimulator->GetShardCount());
					});
				}
#endif
			}
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			const size_t shardZeroSizeBefore = auditRoot
				? _ToShards[0].GetSize()
				: 0;
#endif
			_ToShards[0].push_back(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			if(auditRoot)
			{
				const size_t shardZeroSizeAfter =
					_ToShards[0].GetSize();
				const uint64_t delta =
					shardZeroSizeAfter >= shardZeroSizeBefore
						? static_cast<uint64_t>(
							shardZeroSizeAfter -
								shardZeroSizeBefore)
						: std::numeric_limits<
							uint64_t>::max();
				if(observedPhysicalCount <=
					std::numeric_limits<uint64_t>::max() -
						delta)
				{
					observedPhysicalCount += delta;
				}
				else
				{
					observedPhysicalCount =
						std::numeric_limits<uint64_t>::max();
				}
				exactlyOnePerDestination =
					exactlyOnePerDestination &&
					delta == 1 &&
					shardZeroSizeAfter != 0 &&
					_ToShards[0][shardZeroSizeAfter - 1] ==
						t;
			}
#endif
			APPEND_TRACE(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			metrics->physicalRelayRoutes.fetch_add(
				_ToShards.GetSize(),
				std::memory_order_relaxed);
			metrics->broadcastPhysicalClones.fetch_add(
				_ToShards.GetSize() > 0
					? _ToShards.GetSize() - 1
					: 0,
				std::memory_order_relaxed);
			if(auditRoot)
			{
				uint64_t logicalOccurrences = 0;
				for(auto& shardBuffer : _ToShards)
				{
					for(SimuTxn* routed : shardBuffer)
					{
						if(routed == t)
							++logicalOccurrences;
					}
				}
				audit->CheckClassifiedExactlyOnce(
					*auditRoot,
					logicalOccurrences);
				audit->CheckBroadcastClones(
					*auditRoot,
					static_cast<uint32_t>(_ToShards.GetSize()),
					observedPhysicalCount,
					exactlyOnePerDestination);
			}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
			{
				auto* relayTraceCollector =
					_pSimulator->GetRelayTraceCollector();
				RunRelayTraceObservation(
					relayTraceCollector,
					"broadcast relay route recording",
					[&]()
				{
					if (relayTraceCollector == nullptr)
						return;
					relayTraceCollector->RecordRoute(
						t,
						0,
						relay_trace::RouteKind::
							AllShardsBroadcast,
						_pSimulator->GetShardCount());
				});
			}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			}
#endif
			break;
		default:
			{
				const uint32_t expectedDestination =
					_pSimulator->GetShardIndex(t->Target);
				const uint32_t si = expectedDestination;
				APPEND_TRACE(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
				metrics->physicalRelayRoutes.fetch_add(
					1,
					std::memory_order_relaxed);
#endif
				if (t->OriginateShardIndex == si)
				{
					_pShard->PushIntraRelay(t);
					_pSimulator->OnTxnPushed();
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
					{
						auto* relayTraceCollector =
							_pSimulator->GetRelayTraceCollector();
						RunRelayTraceObservation(
							relayTraceCollector,
							"intra-shard relay route recording",
							[&]()
						{
							if (relayTraceCollector == nullptr)
								return;
							relayTraceCollector->RecordRoute(
								t,
								si,
								relay_trace::RouteKind::
									IntraShard,
								_pSimulator->GetShardCount());
						});
					}
#endif
					continue;
				}
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
				const size_t routeSizeBefore = auditRoot
					? _ToShards[si].GetSize()
					: 0;
#endif
				_ToShards[si].push_back(t);
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
				if(auditRoot)
				{
					const size_t routeSizeAfter =
						_ToShards[si].GetSize();
					uint64_t occurrences = 0;
					uint32_t actualDestination =
						std::numeric_limits<uint32_t>::max();
					for(uint32_t destination = 0;
						destination < _ToShards.GetSize();
						++destination)
					{
						for(SimuTxn* routed :
							_ToShards[destination])
						{
							if(routed == t)
							{
								++occurrences;
								actualDestination = destination;
							}
						}
					}
					if(routeSizeAfter < routeSizeBefore ||
						routeSizeAfter - routeSizeBefore != 1)
					{
						occurrences = routeSizeAfter >=
							routeSizeBefore
							? routeSizeAfter -
								routeSizeBefore
							: std::numeric_limits<
								uint64_t>::max();
						actualDestination =
							std::numeric_limits<
								uint32_t>::max();
					}
					audit->CheckClassifiedExactlyOnce(
						*auditRoot,
						occurrences);
					audit->CheckDestinationShard(
						*auditRoot,
						expectedDestination,
						actualDestination);
				}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
				{
					auto* relayTraceCollector =
						_pSimulator->GetRelayTraceCollector();
					RunRelayTraceObservation(
						relayTraceCollector,
						"cross-shard relay route recording",
						[&]()
					{
						if (relayTraceCollector == nullptr)
							return;
						relayTraceCollector->RecordRoute(
							t,
							si,
							relay_trace::RouteKind::
								CrossShard,
							_pSimulator->GetShardCount());
					});
				}
#endif
			}
		}
	}

	txns.ShrinkSize(0);
#undef APPEND_TRACE
	return logicalRelayCount;
}

void RelayEmission::Dispatch()
{
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint64_t dispatchBeginNs = RelayOptimizationNowNs();
#endif
	if(_ToGlobal.GetSize())
	{
		_pSimulator->GetGlobalShard()->PushRelayTxn(_ToGlobal, (uint32_t)_ToGlobal.GetSize());
		_ToGlobal.ShrinkSize(0);
	}

	for(uint32_t i=0; i<_ToShards.GetSize(); i++)
	{
		auto& txns = _ToShards[i];
		if(txns.GetSize())
		{
			_pSimulator->GetShard(i)->PushRelayTxn(txns, (uint32_t)txns.GetSize());
			txns.ShrinkSize(0);
		}
	}
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	_pSimulator->GetRelayOptimizationMetrics()->
		dispatchTimeNs.fetch_add(
			RelayOptimizationNowNs() - dispatchBeginNs,
			std::memory_order_relaxed);
#endif
}

void SimuShard::AppendToTxnTrace(SimuTxn* origin, SimuTxn* relay)
{
	rt::BufferEx<SimuTxn*> tmp;
	if(_TxnTrace.has(origin))
		tmp = _TxnTrace.get(origin);

	tmp.push_back(relay);
	_TxnTrace[origin] = tmp;
}

const rt::BufferEx<SimuTxn*> SimuShard::GetTxnTrace(SimuTxn* txn) const
{
	if(_TxnTrace.has(txn))
		return _TxnTrace.get(txn);
	else
		return rt::BufferEx<SimuTxn*>();
}

uint32_t SimuShard::GetCount() const
{
	ASSERT(_pTxn);
	return _pTxn->HasNativeTokensSupplied()?_pTxn->GetNativeTokenSupplied().GetCount():0;
}

rvm::ConstNativeToken SimuShard::Get(uint32_t idx) const
{
	ASSERT(_pTxn);
	rvm::ConstNativeToken ret = { rvm::TokenIdInvalid, {nullptr, 0} };
	if (_pTxn->HasNativeTokensSupplied() && _pTxn->GetNativeTokenSupplied().GetCount() > idx)
	{
		const rvm::Coins& tk = _pTxn->GetNativeTokenSupplied().Get(idx);
		ret.Token = tk.GetId();
		auto& a = tk.GetAmount();
		if (a.IsPositive())
		{
			ret.Amount.BlockCount = a.GetBlockCount();
			ret.Amount.Blocks = a._Data;
			ret.Amount.Sign = a.GetSign();
		}
	}

	return ret;
}

void SimuShard::_Execute(SimuTxn* t)
{
	_pTxn = t;
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	auto* relayOptimizationMetrics =
		_pSimulator->GetRelayOptimizationMetrics();
	auto* relayOptimizationAudit =
		_pSimulator->GetRelayOptimizationAudit();
	std::optional<uint64_t> relayOptimizationAuditRoot;
	relay_plan::RelayPlanLookupResult relayOptimizationPlan;
	relay_optimization::ReserveDecision relayReserveDecision;

	ASSERT(_RelayEmitted.GetSize() == 0);
	if(_pTxn->IsRelay())
	{
		relayOptimizationMetrics->relayExecutions.fetch_add(
			1,
			std::memory_order_relaxed);
	}
	if(relayOptimizationAudit &&
		_pTxn->Type != rvm::InvokeContextType::System &&
		_pTxn->GetEngineId() == rvm::EngineId::PREDA_NATIVE)
	{
		if(_pTxn->IsRelay())
		{
			relayOptimizationAuditRoot =
				relayOptimizationAudit->RootForTransaction(_pTxn);
		}
		else
		{
			const auto issuanceOrdinal =
				relayOptimizationAudit->
					ConsumeSourceIssuance(_pTxn);
			if(issuanceOrdinal)
			{
				const uint64_t canonicalRootIdentity =
					RelayAuditRootIdentity(
						*relayOptimizationAudit,
						*_pTxn);
				const uint64_t rootIdentity =
					relayOptimizationAudit->
						RootIdentityForIssuance(
							canonicalRootIdentity,
							*issuanceOrdinal);
				if(relayOptimizationAudit->ShouldSample(
					rootIdentity))
				{
					relayOptimizationAudit->
						RegisterSampledTransaction(
							_pTxn,
							rootIdentity,
							true);
					relayOptimizationAuditRoot =
						rootIdentity;
				}
			}
		}
	}

	const auto& relayOptimizationConfig =
		_pSimulator->GetRelayOptimizationConfig();
	if(relayOptimizationConfig.UsesRelayPlan() &&
		_pTxn->Type != rvm::InvokeContextType::System &&
		_pTxn->GetEngineId() == rvm::EngineId::PREDA_NATIVE)
	{
		const auto* deployed = GetContractDeployed(
			rvm::CONTRACT_UNSET_SCOPE(_pTxn->Contract));
		if(deployed)
		{
			const uint64_t lookupBeginNs =
				RelayOptimizationNowNs();
			relayOptimizationPlan =
				_pSimulator->LookupRelayPlan(
					deployed->Module,
					static_cast<uint32_t>(_pTxn->Op));
			relayOptimizationMetrics->planLookupTimeNs.fetch_add(
				RelayOptimizationNowNs() - lookupBeginNs,
				std::memory_order_relaxed);
		}

		const relay_plan::FunctionRelayPlan* functionPlan =
			relayOptimizationPlan.function;
		relayReserveDecision =
			relay_optimization::SelectDirectRelayReserve(
				functionPlan,
				_RelayEmitted.GetSize(),
				relayOptimizationConfig.maxRelayReserve);
		if(relayReserveDecision.ShouldReserve())
		{
			relayOptimizationMetrics->
				optimizationEligibleInvocations.fetch_add(
					1,
					std::memory_order_relaxed);
			const uint64_t reserveBeginNs =
				RelayOptimizationNowNs();
			relayOptimizationMetrics->
				relayBufferReserveCalls.fetch_add(
					1,
					std::memory_order_relaxed);
			relayOptimizationMetrics->
				relayBufferReservedElements.fetch_add(
					relayReserveDecision.addition,
					std::memory_order_relaxed);
			if(!_RelayEmitted.reserve(
				relayReserveDecision.requiredCapacity))
			{
				relayOptimizationMetrics->
					relayBufferReserveFailures.fetch_add(
						1,
						std::memory_order_relaxed);
			}
			if(functionPlan)
			{
				_TxnEmitted.ReserveForPlan(
					*functionPlan,
					relayReserveDecision.addition,
					relayOptimizationConfig.maxRelayReserve);
			}
			relayOptimizationMetrics->reserveTimeNs.fetch_add(
				RelayOptimizationNowNs() - reserveBeginNs,
				std::memory_order_relaxed);
		}
		else if(relayReserveDecision.HasTrustedCount() &&
			relayReserveDecision.addition == 0)
		{
			// A trusted zero count is eligible, but the required fast path is
			// deliberately a no-op: no allocation and no reserve metric.
			relayOptimizationMetrics->
				optimizationEligibleInvocations.fetch_add(
					1,
					std::memory_order_relaxed);
		}
		else
		{
			relayOptimizationMetrics->
				optimizationFallbackInvocations.fetch_add(
					1,
					std::memory_order_relaxed);
			switch(relayReserveDecision.kind)
			{
			case relay_optimization::ReserveDecisionKind::SkippedLimit:
			case relay_optimization::ReserveDecisionKind::SkippedOverflow:
				relayOptimizationMetrics->
					relayBufferReserveSkippedLimit.fetch_add(
						1,
						std::memory_order_relaxed);
				break;
			case relay_optimization::ReserveDecisionKind::SkippedUnknown:
			case relay_optimization::ReserveDecisionKind::SkippedNoPlan:
			case relay_optimization::ReserveDecisionKind::SkippedIneligible:
				relayOptimizationMetrics->
					relayBufferReserveSkippedUnknown.fetch_add(
						1,
						std::memory_order_relaxed);
				break;
			default:
				break;
			}
			}
		}
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	std::optional<relay_trace::RuntimeTxnTraceContext>
		relayTraceExecution;
	rvm::ContractModuleID relayTraceModule{};
	auto* relayTraceCollector =
		_pSimulator->GetRelayTraceCollector();
	RunRelayTraceObservation(
		relayTraceCollector,
		"execution trace begin",
		[&]()
	{
		if (relayTraceCollector == nullptr ||
			_pTxn->Type == rvm::InvokeContextType::System ||
			_pTxn->GetEngineId() != rvm::EngineId::PREDA_NATIVE)
		{
			return;
		}

		const auto* deployed = GetContractDeployed(
			rvm::CONTRACT_UNSET_SCOPE(_pTxn->Contract));
		if (deployed != nullptr)
		{
			relayTraceModule = deployed->Module;
			std::string sourceFunctionId;
			_pSimulator->ResolveRelayTraceFunction(
				relayTraceModule,
				static_cast<uint32_t>(_pTxn->Op),
				sourceFunctionId);

			relay_trace::ExecutionBeginInput input;
			input.transaction = _pTxn;
			input.contractInvoke = _pTxn->Contract;
			input.opcode = static_cast<uint32_t>(_pTxn->Op);
			input.moduleId =
				GetRelayTraceModuleIdentity(relayTraceModule);
			input.sourceFunctionId =
				std::move(sourceFunctionId);
			input.ownerShard = _ShardIndex;
			input.isRelay = _pTxn->IsRelay();
			relayTraceExecution =
				relayTraceCollector->BeginExecution(input);
		}
	});
#endif
	rvm::ConstData args = _pTxn->GetArguments();
	ConfirmTxn& cTxn = _TxnExecuted.push_back();
	rvm::InvokeResult& ret = cTxn.Result;
	cTxn.Txn = _pTxn;

	rvm::ExecutionUnit* pexec = nullptr;
	if(_pTxn->Type != rvm::InvokeContextType::System)
	{
		pexec = _ExecUnits.Get(_pTxn->GetEngineId());
		if(pexec)
		{
			if(_pTxn->HasNativeTokensSupplied())
			{	// withdraw native assets supply
				ASSERT(_pTxn->Type == rvm::InvokeContextType::Normal);
				auto& withdraw = _pTxn->GetNativeTokenSupplied();
				uint32_t withdraw_count = withdraw.GetCount();
				if(withdraw_count)
				{
					ASSERT(_pTxn->Target.target_size == sizeof(rvm::Address));
					CoreContractState<CORE_WALLET> s = _AddressStates.Get({ _pTxn->Target.addr, -CORE_WALLET });
					if(s.IsEmpty())
					{
						rt::Zero(ret);
						ret.Code = rvm::InvokeErrorCode::InsufficientFunds;
						goto POST_INVOKE;
					}

					rvm::CoinsWalletMutable next = s.DeriveMutable();
					for(uint32_t i=0; i<withdraw_count; i++)
					{
						if(!next.Withdraw(withdraw.Get(i)))
						{
							rt::Zero(ret);
							ret.Code = rvm::InvokeErrorCode::InsufficientFunds;
							goto POST_INVOKE;
						}
					}

					CommitCoreState<CORE_WALLET>(next);
				}
			}

			ret = pexec->Invoke(this, (uint32_t)_pTxn->Gas, _pTxn->Contract, _pTxn->Op, &args);
			if(ret.TokenResidual)
			{
				thread_local rt::String str;
				uint32_t co = ret.TokenResidual->GetCount();

				str.Empty();
				for(uint32_t i=0; i<co; i++)
				{
					const rvm::ConstNativeToken cnt = ret.TokenResidual->Get(i);
					ext::BigNumRef bRef = ext::BigNumRef(cnt.Amount.Sign, cnt.Amount.Blocks, cnt.Amount.BlockCount);
					ASSERT(!cnt.Amount.Sign);
					ext::_details::BN_ToString(bRef, str, 10);
					str += ' ';
					str += rvm::TokenIdToSymbol(cnt.Token);
					if(i<co-1)
					{
						str += ',';
						str += ' ';
					}
					ext::BigNumMutable BNTmp;
					BNTmp = bRef;
					cTxn.Residual.push_back({ cnt.Token, BNTmp });
				}

				_LOGC_VERBOSE("[PRD]: Execution Residual: "<<str);
				_SafeRelease(ret.TokenResidual);
			}
		}
		else
		{
			rt::Zero(ret);
			ret.Code = rvm::InvokeErrorCode::ExecutionEngineUnavailable;
			_LOG_WARNING("[PRD]: Engine " << rt::EnumStringify(_pTxn->GetEngineId()) << " is not available");
		}
	}	
	else
	{
		rvm::ConstData args = _pTxn->GetArguments();
		ret = _pSimulator->DeployFromStatement(args);
	}

POST_INVOKE:
	if(IsGlobal())
		_pGlobalShard->_OnGlobalTransactionExecuted(ret.Code == rvm::InvokeErrorCode::Success);

	if(ret.Code != rvm::InvokeErrorCode::Success)
	{
		if((int)ret.SubCodeLow > 0)
		{
			rvm::StringStreamImpl str;
			if(pexec)
				pexec->GetExceptionMessage(ret.SubCodeLow, &str);
			else
			{
				std::string s = std::move(std::to_string(ret.SubCodeLow));
				str.Append(s.c_str(), uint32_t(s.length()));
			}
			_LOG_WARNING("[PRD]: Engine invoke error. Error Message: " << rt::EnumStringify(ret.Code) << " (" << (rt::String_Ref)str << ")");
		}
		else
			_LOG_WARNING("[PRD]: Engine invoke error. Error Message: " << rt::EnumStringify(ret.Code));
	}

	_pTxn->Height = _BlockHeight;
	ASSERT(_ShardIndex <= 65535)
	_pTxn->ShardIndex = uint16_t(_ShardIndex);
	_pTxn->ShardOrder = _ShardOrder;

	uint64_t remainingGas = _pTxn->Gas - ret.GasBurnt;
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	const uint32_t actualDirectRelayCount =
		static_cast<uint32_t>(_RelayEmitted.GetSize());
	if(relayReserveDecision.ShouldReserve() &&
		actualDirectRelayCount > relayReserveDecision.addition)
	{
		relayOptimizationMetrics->
			relayBufferCapacityMisses.fetch_add(
				1,
				std::memory_order_relaxed);
	}
	if(relayOptimizationAuditRoot &&
		ret.Code == rvm::InvokeErrorCode::Success &&
		relayReserveDecision.HasTrustedCount())
	{
		std::optional<uint64_t> exact;
		std::optional<uint64_t> upperBound;
		if(relayReserveDecision.sourceKind ==
			relay_plan::CountPlanKind::ExactConstant)
		{
			exact = relayReserveDecision.addition;
		}
		else
		{
			upperBound = relayReserveDecision.addition;
		}
		relayOptimizationAudit->CheckDirectRelayCount(
			*relayOptimizationAuditRoot,
			exact,
			upperBound,
			actualDirectRelayCount);
	}
	uint32_t routedLogicalRelayCount = 0;
	if(actualDirectRelayCount)
	{
		const uint64_t routingBeginNs =
			RelayOptimizationNowNs();
		routedLogicalRelayCount =
			_TxnEmitted.Collect(
				t,
				_RelayEmitted,
				remainingGas);
		relayOptimizationMetrics->routingTimeNs.fetch_add(
			RelayOptimizationNowNs() - routingBeginNs,
			std::memory_order_relaxed);
	}
	if(relayOptimizationAuditRoot &&
		routedLogicalRelayCount != actualDirectRelayCount)
	{
		relayOptimizationAudit->FailSample(
			relay_optimization::AuditCheckKind::
				LogicalClassification,
			*relayOptimizationAuditRoot,
			actualDirectRelayCount,
			routedLogicalRelayCount,
			"logical relay transfer to routing lost or "
			"duplicated an emission");
	}
#else
	if(_RelayEmitted.GetSize())
		_TxnEmitted.Collect(t, _RelayEmitted, remainingGas);
#endif
	_TotalGasBurnt += _pTxn->Gas - remainingGas;

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	if (relayTraceExecution)
	{
		auto* relayTraceCollector =
			_pSimulator->GetRelayTraceCollector();
		RunRelayTraceObservation(
			relayTraceCollector,
			"execution trace validation",
			[&]()
		{
			_pSimulator->ValidateRelayTraceExecution(
				relayTraceModule,
				*relayTraceExecution,
				ret.Code == rvm::InvokeErrorCode::Success);
		});
		RunRelayTraceObservation(
			relayTraceCollector,
			"execution trace end",
			[&]()
		{
			if (relayTraceCollector != nullptr)
			{
				relayTraceCollector->EndExecution(
					t,
					true,
					ret.Code == rvm::InvokeErrorCode::Success);
			}
		});
		_RelayTraceMarkerStack.clear();
	}
#endif

#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
	if(relayOptimizationAudit)
		relayOptimizationAudit->ForgetTransaction(_pTxn);
#endif

	//_LOG("Gas Burnt: " << _pTxn->Gas - remainingGas);
}

void SimuShard::_BlockCreationRoutine()
{
#if defined(__linux__) && defined(AFFINITY_SET)
        if(!IsGlobal())
        {
		    struct sched_param param;
            param.sched_priority = 99; // Setting priority.

    	    // set the scheduling parameters
    	    int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    	    if (ret != 0) {
//    	         printf("pthread_setschedparam returned %d\n", ret);
    	    }
		    cpu_set_t cpuset;
		    pthread_t thread = pthread_self();
		    int core = this->_ShardIndex;
		    CPU_ZERO(&cpuset);
		    CPU_SET(core, &cpuset);
		    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        }
#endif
	while(!_BlockCreator.WantExit())
	{
		bool step = _pSimulator->IsChainStepping();
		bool async = _pSimulator->IsShardingAsync() && !IsGlobal();

		if(	!async || step || _pSimulator->IsChainPaused() ||
			(_PendingRelayTxns.IsEmpty() && _PendingTxns.IsEmpty())
		)
		{	_GoNextBlock.WaitSignal();
			_GoNextBlock.Reset();
		}

		if(_BlockCreator.WantExit())
			return;

		step = _pSimulator->IsChainStepping();
		uint64_t gas_limit = IsGlobal() ? _pSimulator->GetScriptGlobalGasLimit() : _pSimulator->GetScriptGasLimit();

		// start a new block
		_TotalGasBurnt = 0;
		_PrevBlockHash = _BlockHash;
		ASSERT(_TxnExecuted.GetSize() == 0);

		// executing deferred txn from previous block
		{
			for(auto t : _TxnEmitted.GetDeferredTxns())
			{
				_Execute(t); // execute a defer txn
			}

			_TxnEmitted.ClearDeferredTxns();
		}

		// executing txns
		// relay txns first
		while (SimuTxn* t = _TotalGasBurnt < gas_limit ?
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
			_PopRelayTxn()
#else
			_PendingRelayTxns.Pop()
#endif
			: nullptr)
		{
			if (!async)
			{
				// there shouldn't be txns from larger height because in sync mode all shards grow at the same time
				ASSERT(t->OriginateHeight <= _BlockHeight);
				// a relay txn shouldn't be included in blocks of the same height as its originate height, unless it's a relay from global shard
				if (t->OriginateHeight == _BlockHeight && t->OriginateShardIndex != rvm::GlobalShard)
				{
					// push the txn back to the front of the queue
					_PendingRelayTxns.Push_Front(t);
					// txns in _PendingRelayTxns are always in increasing order of OriginateHeight; and for txns of the same height, those from global shard are enqueued before the txns from other shards
					// therefore, once a txn with OriginateHeight of _BlockHeight and OriginateShardIndex of non-global is encountered, the rest of the queue should all be skipped for the current block
					break;
				}
			}
			ASSERT(_RelayEmitted.GetSize() == 0);
			_Execute(t); // execute a txn
		}

		// normal txns second
#ifdef VERIFY_SIG
		oxd::SecuritySuite ss(oxd::SEC_SUITE_ED25519);
#endif
		while (SimuTxn* t = _TotalGasBurnt < gas_limit ? _PendingTxns.Pop() : nullptr)
		{
			ASSERT(_RelayEmitted.GetSize() == 0);
#ifdef VERIFY_SIG
			if(rvm::Scope::Address == t->GetScope() && !ss.VerifySignature( &(t->pk[0]), &(t->sig[0]), &t->SerializedData, t->ArgsSerializedSize)) {
				std::cout << "verify failed" << std::endl;
			}
#endif
			_Execute(t); // execute a txn
		}

		// intra-relays last
		while (SimuTxn* t = _IntraRelayTxns.Pop())
			_Execute(t);

		// finalize a block
		auto* block = SimuBlock::Create((uint32_t)_TxnExecuted.GetSize());
		block->Height = _BlockHeight;
		block->PrevBlock = _PrevBlockHash;
		block->Timestamp = _GetBlockTime();
		block->Miner = _pSimulator->GetMiner();
		block->ShardId = _ShardIndex;
		block->TxnCount = (uint32_t)_TxnExecuted.GetSize();
		_TxnExecuted.CopyTo(block->Txns);
		*(uint64_t*)&_BlockHash = os::crc64(block, offsetof(SimuBlock, Txns));
		block->TotalGas = _TotalGasBurnt;
		_Chain.push_back(block);

		// dispatch relay
		_TxnEmitted.Dispatch();
		_pSimulator->OnTxnPushed(uint32_t(_TxnEmitted.GetDeferredTxns().GetSize()));		// otherwise _PendingTxnCount would be incorrect and chain step might step due if there are no other txns pending

		// clean up
		_ConfirmedTxnCount += (uint32_t)_TxnExecuted.GetSize();
		if(_TxnExecuted.GetSize())
		{
			_pSimulator->OnTxnsConfirmed((uint32_t)_TxnExecuted.GetSize());
			_TxnExecuted.ShrinkSize(0);
		}
		_ReturnVal.ShrinkSize(0);
		_BlockHeight++;

		//_BlockCreator.WantExit() = _pSimulator->GetTerminationSignal();

		// continue to next block
		if(IsGlobal())
		{	// trigger all shards
			ASSERT(!async);
			for(uint32_t i=0; i<_pSimulator->GetShardCount(); i++)
				_pSimulator->GetShard(i)->Step();
		}
		else
		{
			if(!async)
			{
				bool trigger_global = _pSimulator->CompleteShardExecution();
				if(trigger_global)
				{
					if(_pSimulator->GetPendingTxnsCount() && !step && !_pSimulator->IsChainPaused())
						_pGlobalShard->Step();
					else
						_pSimulator->SetChainIdle();
				}
			}
			else{
				if(_pSimulator->GetPendingTxnsCount() && !step && !_pSimulator->IsChainPaused())
					_pGlobalShard->Step();
				else
					_pSimulator->SetChainIdle();
			}
		}
	}
}

void SimuShard::LogInfo()
{
	rt::tos::Number gi(_ShardIndex);
	if(IsGlobal())(rt::String_Ref&)gi = "g";

	_LOG(	"[PRD]: " <<
			"Shd#"<<gi<<":\t"<<
			"h:"<<_BlockHeight<<" "
			"txn:"<<_PendingTxns.GetSize()<<'/'<<_PendingRelayTxns.GetSize()<<'/'<<_ConfirmedTxnCount<<" "
			"addr:"<<_AddressStates.GetSize()
	);
}

void SimuShard::Step()
{
	_GoNextBlock.Set();
}
bool SimuShard::JsonifyBlocks(rt::Json& append, int64_t height)
{
	if(!_Chain.GetSize() || height >= (int)_Chain.GetSize())
	{
		_LOG("[PRD] Line " << _pSimulator->GetLineNum() << ": Block index is out of range")
		return false;
	}
	uint64_t starting_idx = height < 0 ? 0 : height;
	uint64_t ending_idx = height < 0 ? _Chain.GetSize() : height + 1;
	rt::String shard_index = IsGlobal()? "#g" : rt::String("#") + GetShardIndex();
	for(;starting_idx < ending_idx; starting_idx++)
	{
		auto s1 = append.ScopeAppendingElement();
		SimuBlock* blk = _Chain[starting_idx];
		rt::String prev_blk_hash;
		prev_blk_hash.SetLength(os::Base32EncodeLength(RVM_HASH_SIZE));
		os::Base32CrockfordEncodeLowercase(prev_blk_hash.Begin(), &blk->PrevBlock, RVM_HASH_SIZE);
		append.Object((
			(J(Height) = blk->Height),
			(J(PrevBlock) = prev_blk_hash),
			(J(ShardIndex) = shard_index),
			(J(Timestamp) = blk->Timestamp),
			(J(Miner) = oxd::SecureAddress::String(blk->Miner)),
			(J(TxnCount) = blk->TxnCount)
		));
		if(blk->TxnCount > 0)
		{
			auto s2 = append.ScopeAppendingKey("ConfirmTxn");
			append.Array();
			for(uint32_t i = 0; i < blk->TxnCount; i++)
			{
				auto& txn = _Chain[starting_idx]->Txns[i];
				txn.Jsonify(_pSimulator->GetEngine(txn.Txn->GetEngineId()), append);
			}
		}
	}

	return true;
}

bool SimuShard::JsonifyAllAddressStates(rt::Json& append, const rt::BufferEx<User>& Users, rvm::ContractId cid)
{
	if(!_AddressStates.GetSize())
	{
		return false;
	}
	for(uint32_t id = 0; id < Users.GetSize(); ++id)
		_AddressStates.AddressStateJsonify(_pSimulator->GetAllEngines(), append, id, cid, GetShardIndex(), &Users[id].Addr);
	return true;
}

bool SimuShard::JsonifyAddressState(rt::Json& append, const rvm::Address* targetAddr, uint32_t userId, rvm::ContractId cid)
{
	if(rvm::CONTRACT_ENGINE(cid) == rvm::EngineId::SOLIDITY_EVM)
	{
		rvm::ContractInvokeId ciid = rvm::CONTRACT_SET_SCOPE_BUILD(cid, rvm::Scope::Address, rvm::BuildNumInit);
		rvm::StringStreamImpl str_impl;
		std::vector<uint8_t> data(sizeof(rvm::ChainStates*) + sizeof(rvm::Address));
		auto chain_state_addr = dynamic_cast<rvm::ChainStates*>(this);
		std::memcpy(data.data(), &chain_state_addr, sizeof(rvm::ChainStates*));
		std::memcpy(data.data() + sizeof(rvm::ChainStates*), targetAddr, sizeof(rvm::Address));
		rvm::ConstData const_data{data.data(), sizeof(rvm::Address)};
		if (!_pSimulator->GetAllEngines()[(uint8_t)rvm::EngineId::SOLIDITY_EVM].pEngine->StateJsonify(ciid, &const_data, &str_impl))
			return false;
		nlohmann::json json;
		rt::tos::Base32CrockfordLowercaseOnStack<> addrStr(targetAddr, sizeof(rvm::ConstAddress));
		json["Address"] = std::string(addrStr.Begin());
		json["AddressIndex"] = userId;
		json["ShardIndex"] = _ShardIndex;
		json["States"].push_back(nlohmann::json::parse(str_impl.GetString().StrPtr));
		nlohmann::json json_out = nlohmann::json::parse(append.GetInternalString().GetString());
		json_out.push_back(json);
		append.GetInternalString() = rt::String(json_out.dump().c_str());
	}
	else
	{
		if(!_AddressStates.GetSize())
			return false;
		_AddressStates.AddressStateJsonify(_pSimulator->GetAllEngines(), append, userId, cid, GetShardIndex(), targetAddr);
	}
	return true;
}

void SimuShard::JsonifyAddressStateWithoutGrouping(rt::Json& append, rvm::ContractId cid)
{
	_AddressStates.ContractStateJsonify(_pSimulator->GetAllEngines(), append, cid);
}

bool SimuShard::JsonifyShardState(rt::Json& append, rvm::ContractId cid)
{
	if(!_ShardStates.ShardStateJsonify(_pSimulator->GetAllEngines(), append, cid, GetShardIndex()))
		return false;
	if(!_ShardKeyedStates.ScatteredMapJsonify(_pSimulator->GetAllEngines(), append.GetInternalString(), GetShardIndex()))
		return false;
	if(rvm::CONTRACT_ENGINE(cid) == rvm::EngineId::SOLIDITY_EVM)
	{
		rvm::ContractInvokeId ciid = rvm::CONTRACT_SET_SCOPE_BUILD(cid, IsGlobal() ? rvm::Scope::Global : rvm::Scope::Shard, rvm::BuildNumInit);
		rvm::StringStreamImpl str_impl;
		std::vector<uint8_t> data(sizeof(rvm::ChainStates*));
		auto chain_state_addr = dynamic_cast<rvm::ChainStates*>(this);
		std::memcpy(data.data(), &chain_state_addr, sizeof(rvm::ChainStates*));
		rvm::ConstData const_data{data.data(), 0};
		if (!_pSimulator->GetAllEngines()[(uint8_t)rvm::EngineId::SOLIDITY_EVM].pEngine->StateJsonify(ciid, &const_data, &str_impl))
			return false;
		nlohmann::json json = nlohmann::json::parse(append.GetInternalString().GetString());
		json.back()["States"].push_back(nlohmann::json::parse(str_impl.GetString().StrPtr));
		append.GetInternalString() = rt::String(json.dump().c_str());
	}
	return true;
}

bool SimuShard::ConfirmedTxnJsonify(const SimuTxn* txn, rt::Json& append) const
{
	uint64_t height = txn->Height;
	if(height < _Chain.GetSize())
	{
		SimuBlock* blk = _Chain[height];
		for(uint32_t i = 0; i < blk->TxnCount; i++)
		{
			if(blk->Txns[i].Txn == txn)
			{
				blk->Txns[i].Jsonify(_pSimulator->GetEngine(blk->Txns[i].Txn->GetEngineId()), append);
				return true;
			}
		}
	}
	return false;
}
bool SimuShard::JsonifyScopeState(rt::Json& append, SimuAddressContract key, bool allState)
{
	return _AddressStates.ScopeStateJsonify(_pSimulator->GetAllEngines(), append, key, _ShardIndex, allState);
}

void SimuShard::JsonifyProfile(rt::Json& append) const
{
	rt::String sh = IsGlobal() ? "#g" : rt::String("#") + GetShardIndex();
	append.Object((J(ShardIndex) = sh,
		J(BlockHeight) = GetBlockHeight()));
}
} // namespace oxd
