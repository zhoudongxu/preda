#include "../shard_data.h"

#include <atomic>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef RPREDA_PENDING_TXNS_BATCH_TESTING

namespace oxd {
namespace relay_optimization {
namespace tests {
namespace {

struct TestState
{
	int failures = 0;

	void Expect(bool condition, const std::string& message)
	{
		if(condition)
			return;
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
};

class PendingTxnsProbe : public PendingTxns
{
public:
	void LockForEmptyBatchTest()
	{
		_Mutex.lock();
	}

	void UnlockForEmptyBatchTest()
	{
		_Mutex.unlock();
	}
};

SimuTxn* MakeRelay(uint32_t id)
{
	SimuTxn* txn = SimuTxn::Create(sizeof(id), 0);
	txn->Flag = TXN_RELAY;
	txn->Type = rvm::InvokeContextType::RelayInbound;
	std::memcpy(txn->SerializedData, &id, sizeof(id));
	return txn;
}

std::vector<SimuTxn*> Drain(PendingTxns& queue)
{
	std::vector<SimuTxn*> result;
	while(SimuTxn* txn = queue.Pop())
		result.push_back(txn);
	return result;
}

void ReleaseOwned(const std::vector<SimuTxn*>& transactions)
{
	for(SimuTxn* txn : transactions)
		txn->Release();
}

void TestEmptyBatchDoesNotLock(TestState& state)
{
	PendingTxnsProbe queue;
	queue.LockForEmptyBatchTest();
	const PendingPushResult result = queue.PushBatch(nullptr, 0);
	queue.UnlockForEmptyBatchTest();

	state.Expect(result.committed, "empty batch succeeds");
	state.Expect(!result.wasEmpty, "empty batch does not observe queue state");
	state.Expect(result.inserted == 0, "empty batch inserts no elements");
	state.Expect(queue.GetSize() == 0, "empty batch leaves queue unchanged");
}

void TestSingleAndMultipleOrder(TestState& state)
{
	{
		PendingTxns queue;
		std::vector<SimuTxn*> owned = {MakeRelay(1)};
		const PendingPushResult result =
			queue.PushBatch(owned.data(), owned.size());
		state.Expect(result.committed, "single batch commits");
		state.Expect(result.wasEmpty, "single batch observes empty queue");
		state.Expect(result.inserted == 1, "single batch reports one insert");
		const std::vector<SimuTxn*> popped = Drain(queue);
		state.Expect(
			popped == owned,
			"single batch preserves the transaction pointer");
		ReleaseOwned(owned);
	}

	{
		PendingTxns queue;
		std::vector<SimuTxn*> owned = {
			MakeRelay(10),
			MakeRelay(11),
			MakeRelay(12),
		};
		const PendingPushResult result =
			queue.PushBatch(owned.data(), owned.size());
		state.Expect(result.committed, "multiple batch commits");
		state.Expect(result.wasEmpty, "multiple batch observes empty queue");
		state.Expect(result.inserted == owned.size(),
			"multiple batch reports every insert");
		const std::vector<SimuTxn*> popped = Drain(queue);
		state.Expect(popped == owned, "multiple batch preserves input order");
		ReleaseOwned(owned);
	}
}

void TestOldElementsAndPushFront(TestState& state)
{
	PendingTxns queue;
	std::vector<SimuTxn*> owned = {
		MakeRelay(20),
		MakeRelay(21),
		MakeRelay(22),
		MakeRelay(23),
	};
	queue.Push(owned[0]);
	SimuTxn* batch[] = {owned[1], owned[2]};
	const PendingPushResult result = queue.PushBatch(batch, 2);
	queue.Push_Front(owned[3]);

	state.Expect(result.committed, "batch after old element commits");
	state.Expect(!result.wasEmpty, "batch sees the existing queue prefix");
	state.Expect(result.inserted == 2, "batch reports appended element count");
	const std::vector<SimuTxn*> popped = Drain(queue);
	const std::vector<SimuTxn*> expected = {
		owned[3],
		owned[0],
		owned[1],
		owned[2],
	};
	state.Expect(
		popped == expected,
		"Push_Front precedes the old prefix and appended batch");
	ReleaseOwned(owned);
}

void TestFailureRollback(TestState& state)
{
	PendingTxns queue;
	std::vector<SimuTxn*> owned = {
		MakeRelay(30),
		MakeRelay(31),
		MakeRelay(32),
	};
	queue.Push(owned[0]);
	SimuTxn* batch[] = {owned[1], owned[2]};

	PendingTxns::FailNextPushBatchForTesting();
	const PendingPushResult failed = queue.PushBatch(batch, 2);
	state.Expect(!failed.committed, "injected allocation failure is reported");
	state.Expect(!failed.wasEmpty, "failure reports the old queue state");
	state.Expect(failed.inserted == 0, "failed batch commits no pointers");
	state.Expect(queue.GetSize() == 1, "failed batch restores old queue size");
	const std::vector<SimuTxn*> afterFailure = Drain(queue);
	state.Expect(
		afterFailure == std::vector<SimuTxn*>{owned[0]},
		"failed batch preserves the exact old queue prefix");

	const PendingPushResult retry = queue.PushBatch(batch, 2);
	state.Expect(retry.committed, "failure injection is one-shot");
	state.Expect(retry.wasEmpty, "retry sees the drained queue as empty");
	state.Expect(retry.inserted == 2, "retry commits the caller-owned pointers");
	const std::vector<SimuTxn*> afterRetry = Drain(queue);
	state.Expect(
		afterRetry == std::vector<SimuTxn*>({owned[1], owned[2]}),
		"caller retains the failed batch and can retry it unchanged");
	ReleaseOwned(owned);
}

void TestMultiProducerBatchesDoNotInterleave(TestState& state)
{
	constexpr size_t ProducerCount = 8;
	constexpr size_t BatchSize = 6;
	PendingTxns queue;
	std::vector<std::vector<SimuTxn*>> batches(ProducerCount);
	std::vector<SimuTxn*> owned;
	std::vector<PendingPushResult> results(ProducerCount);
	for(size_t producer = 0; producer < ProducerCount; ++producer)
	{
		for(size_t index = 0; index < BatchSize; ++index)
		{
			SimuTxn* txn = MakeRelay(
				static_cast<uint32_t>(producer * BatchSize + index));
			batches[producer].push_back(txn);
			owned.push_back(txn);
		}
	}

	std::atomic<bool> start{false};
	std::vector<std::thread> producers;
	for(size_t producer = 0; producer < ProducerCount; ++producer)
	{
		producers.emplace_back([&, producer]()
		{
			while(!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			results[producer] = queue.PushBatch(
				batches[producer].data(),
				static_cast<uint32_t>(batches[producer].size()));
		});
	}
	start.store(true, std::memory_order_release);
	for(std::thread& producer : producers)
		producer.join();

	for(const PendingPushResult& result : results)
	{
		state.Expect(result.committed, "multi-producer batch commits");
		state.Expect(
			result.inserted == BatchSize,
			"multi-producer result reports its complete batch");
	}

	const std::vector<SimuTxn*> popped = Drain(queue);
	state.Expect(
		popped.size() == ProducerCount * BatchSize,
		"multi-producer queue contains every transaction");
	std::unordered_map<SimuTxn*, size_t> position;
	for(size_t index = 0; index < popped.size(); ++index)
		position.emplace(popped[index], index);
	for(const auto& batch : batches)
	{
		const auto first = position.find(batch.front());
		state.Expect(first != position.end(), "batch head is present");
		if(first == position.end())
			continue;
		for(size_t index = 0; index < batch.size(); ++index)
		{
			const auto found = position.find(batch[index]);
			state.Expect(found != position.end(), "batch element is present");
			if(found != position.end())
			{
				state.Expect(
					found->second == first->second + index,
					"one producer's batch remains contiguous and ordered");
			}
		}
	}
	ReleaseOwned(owned);
}

void TestConcurrentConsumerSeesWholeElements(TestState& state)
{
	constexpr size_t ProducerCount = 4;
	constexpr size_t BatchCountPerProducer = 24;
	constexpr size_t BatchSize = 4;
	constexpr size_t Total =
		ProducerCount * BatchCountPerProducer * BatchSize;
	PendingTxns queue;
	std::vector<std::vector<std::vector<SimuTxn*>>> batches(
		ProducerCount);
	std::vector<SimuTxn*> owned;
	for(size_t producer = 0; producer < ProducerCount; ++producer)
	{
		batches[producer].resize(BatchCountPerProducer);
		for(size_t batch = 0; batch < BatchCountPerProducer; ++batch)
		{
			for(size_t index = 0; index < BatchSize; ++index)
			{
				SimuTxn* txn = MakeRelay(
					static_cast<uint32_t>(owned.size()));
				batches[producer][batch].push_back(txn);
				owned.push_back(txn);
			}
		}
	}

	std::atomic<bool> start{false};
	std::atomic<size_t> producersDone{0};
	std::atomic<size_t> failedPushes{0};
	std::vector<SimuTxn*> consumed;
	std::thread consumer([&]()
	{
		while(!start.load(std::memory_order_acquire))
			std::this_thread::yield();
		while(consumed.size() < Total)
		{
			if(SimuTxn* txn = queue.Pop())
			{
				consumed.push_back(txn);
				continue;
			}
			if(producersDone.load(std::memory_order_acquire) ==
				ProducerCount)
			{
				break;
			}
			std::this_thread::yield();
		}
	});

	std::vector<std::thread> producers;
	for(size_t producer = 0; producer < ProducerCount; ++producer)
	{
		producers.emplace_back([&, producer]()
		{
			while(!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			for(auto& batch : batches[producer])
			{
				const PendingPushResult result = queue.PushBatch(
					batch.data(),
					static_cast<uint32_t>(batch.size()));
				if(!result.committed ||
					result.inserted != batch.size())
				{
					failedPushes.fetch_add(1, std::memory_order_relaxed);
				}
			}
			producersDone.fetch_add(1, std::memory_order_release);
		});
	}
	start.store(true, std::memory_order_release);
	for(std::thread& producer : producers)
		producer.join();
	consumer.join();
	const std::vector<SimuTxn*> remaining = Drain(queue);
	consumed.insert(consumed.end(), remaining.begin(), remaining.end());

	state.Expect(
		failedPushes.load(std::memory_order_relaxed) == 0,
		"concurrent producer batches all commit");
	state.Expect(
		consumed.size() == Total,
		"concurrent consumer receives every committed pointer");
	const std::unordered_set<SimuTxn*> unique(
		consumed.begin(),
		consumed.end());
	state.Expect(
		unique.size() == Total,
		"concurrent consumer sees no duplicate pointer");
	for(SimuTxn* txn : owned)
	{
		state.Expect(
			unique.find(txn) != unique.end(),
			"concurrent consumer sees each caller pointer");
	}
	ReleaseOwned(owned);
}

} // namespace

int RunPendingTxnsBatchUnitTests()
{
	TestState state;
	TestEmptyBatchDoesNotLock(state);
	TestSingleAndMultipleOrder(state);
	TestOldElementsAndPushFront(state);
	TestFailureRollback(state);
	TestMultiProducerBatchesDoNotInterleave(state);
	TestConcurrentConsumerSeesWholeElements(state);
	if(state.failures == 0)
		std::cout << "PendingTxns batch unit tests passed\n";
	return state.failures;
}

} // namespace tests
} // namespace relay_optimization
} // namespace oxd

#ifdef RPREDA_PENDING_TXNS_BATCH_STANDALONE_TEST_MAIN
int main()
{
	return oxd::relay_optimization::tests::
		RunPendingTxnsBatchUnitTests() == 0
		? 0
		: 1;
}
#endif

#endif // RPREDA_PENDING_TXNS_BATCH_TESTING
