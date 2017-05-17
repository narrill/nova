#pragma once

#include <vector>
#include <array>
#include "Envelope.h"
#include "Globals.h"
#include "Job.h"
#include "WorkerThread.h"
#include "QueueWrapper.h"

namespace Nova {
	//Queues a set of Runnables
	template<typename ... Runnables>
	static void Push(Runnables&&... runnables) {
		using namespace internal;
		std::array<Envelope, sizeof...(Runnables) - BatchCount<Runnables...>::value> envs;
		std::vector<Envelope> batchEnvs;
		batchEnvs.reserve(BatchCount<Runnables...>::value * 4);
		PackRunnable(envs, batchEnvs, std::forward<Runnables...>(runnables...));
		Push(std::move(envs));
		Push(std::move(batchEnvs));
	}

	//Queues a set of Runnables, invoking the envelope when all have finished
	template<typename ... Runnables>
	static void Push(Envelope & next, Runnables&&... runnables) {
		using namespace internal;
		SealedEnvelope se(next);
		std::array<Envelope, sizeof...(runnables) - BatchCount<Runnables...>::value> envs;
		std::vector<Envelope> batchEnvs;
		batchEnvs.reserve(internal::BatchCount<Runnables...>::value * 4);
		PackRunnable(envs, batchEnvs, std::forward<Runnables...>(runnables...));
		Push(se, std::move(envs));
		Push(se, std::move(batchEnvs));
	}

	//Queues an envelope
	void Push(Envelope&& e);

	//Queues a vector of envelopes
	void Push(std::vector<Envelope> && envs);

	template<unsigned N>
	void Push(std::array<Envelope, N> && envs) {
		internal::Resources::m_queue.Push(std::forward<decltype(envs)>(envs));
	}

	//Loads a set of Runnables, queues them, then pauses the current call stack until they finish
	template<typename ... Runnables>
	static void Call(Runnables&... runnables) {
		using namespace internal;
		std::array<Envelope, sizeof...(Runnables) - BatchCount<Runnables...>::value> envs;
		std::vector<Envelope> batchEnvs;
		PackRunnableNoAlloc(envs, batchEnvs, runnables...);
		Call(std::move(envs), std::move(batchEnvs));
	}

	//Invokes a Callable object once for each value between start (inclusive) and end (exclusive), passing the value to each invocation
	template<typename Callable, typename ... Params>
	static void ParallelFor(Callable callable, BatchIndex start, BatchIndex end, Params... args) {
		Call(MakeBatchJob([&](BatchIndex start, BatchIndex end, Params... args) {
			for (BatchIndex c = start; c < end; c++)
				callable(c, args...);
		}, start, end, args...));
	}

	namespace internal{
		class Resources {
		public:
			static QueueWrapper<Envelope> m_queue;
			static thread_local std::vector<LPVOID> m_availableFibers;
			static thread_local SealedEnvelope * m_callTrigger;
		};

		//Breaks a Runnable off the parameter pack and recurses
		template<typename ... Runnables, unsigned int N>
		static void PackRunnable(std::array<Envelope, N> & envs, std::vector<Envelope> & batchEnvs, Runnables&&... runnables) {
			unsigned int index(0);
			PackRunnable(envs, index, batchEnvs, std::forward<Runnables...>(runnables...));
		}

		//Breaks a Runnable off the parameter pack and recurses
		template<typename Runnable, typename ... Runnables, unsigned int N>
		static void PackRunnable(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, Runnable && runnable, Runnables&&... runnables) {
			PackRunnable(envs, index, batchEnvs, std::forward<Runnable>(runnable));
			PackRunnable(envs, index, batchEnvs, std::forward<Runnables...>(runnables...));
		}

		//Loads a Runnable into an envelope and pushes it to the given vector
		template<typename Runnable, unsigned int N>
		static void PackRunnable(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, Runnable&& runnable) {
			envs[index++] = std::move(Envelope{ std::forward<Runnable>(runnable) });
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector
		template<typename Callable, typename ... Params, unsigned int N>
		static void PackRunnable(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, BatchJob<Callable, Params...> && j) {
			std::vector<Envelope> splitEnvs = SplitBatchJob(std::forward<decltype(j)>(j));
			batchEnvs.insert(batchEnvs.end(), std::make_move_iterator(splitEnvs.begin()), std::make_move_iterator(splitEnvs.end()));
		}

		//Breaks a Runnable off the parameter pack and recurses
		template<typename ... Runnables, unsigned int N>
		static void PackRunnableNoAlloc(std::array<Envelope, N> & envs, std::vector<Envelope> & batchEnvs, Runnables&... runnables) {
			unsigned int index(0);
			PackRunnableNoAlloc(envs, index, batchEnvs, runnables...);
		}

		//Breaks a Runnable off the parameter pack and recurses
		template<typename Runnable, typename ... Runnables, unsigned int N>
		static void PackRunnableNoAlloc(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, Runnable & runnable, Runnables&... runnables) {
			PackRunnableNoAlloc(envs, index, batchEnvs, runnable);
			PackRunnableNoAlloc(envs, index, batchEnvs, runnables...);
		}

		//Loads a Runnable into an envelope and pushes it to the given vector
		template<typename Runnable, unsigned int N>
		static void PackRunnableNoAlloc(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, Runnable& runnable) {
			envs[index++] = { &runnable };
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector
		template<typename Callable, typename ... Params, unsigned int N>
		static void PackRunnableNoAlloc(std::array<Envelope, N> & envs, unsigned & index, std::vector<Envelope> & batchEnvs, BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> splitEnvs = SplitBatchJobNoAlloc(j);
			envs.insert(envs.end(), splitEnvs.begin(), splitEnvs.end());
		}

		//Converts a BatchJob into a vector of Envelopes
		template<typename Callable, typename ... Params>
		static std::vector<Envelope> SplitBatchJob(BatchJob<Callable, Params...> && j) {
			std::vector<Envelope> jobs;
			jobs.reserve(j.GetSections());
			auto* basePtr = new BatchJob<Callable, Params...>(std::forward<decltype(j)>(j));
			SealedEnvelope se(Envelope(&Envelope::NoOp, &Envelope::DeleteRunnable<BatchJob<Callable, Params...>>, basePtr));
			for (unsigned int section = 0; section < j.GetSections(); section++) {
				jobs.emplace_back(basePtr);
				jobs[section].AddSealedEnvelope(se);
			}
			return jobs;
		}

		//Converts a BatchJob into a vector of Envelopes without copying the job to the heap
		template<typename Callable, typename ... Params>
		static std::vector<Envelope> SplitBatchJobNoAlloc(BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> jobs;
			jobs.reserve(j.GetSections());
			for (unsigned int section = 0; section < j.GetSections(); section++) {
				jobs.emplace_back(&j);
			}
			return jobs;
		}

		template<typename... Args> struct BatchCount;

		template<>
		struct BatchCount<> {
			static const int value = 0;
		};

		template<typename ... Params, typename... Args>
		struct BatchCount<BatchJob<Params...>, Args...> {
			static const int value = 1 + BatchCount<Args...>::value;
		};

		template<typename First, typename... Args>
		struct BatchCount<First, Args...> {
			static const int value = BatchCount<Args...>::value;
		};

		template<unsigned N>
		void Push(SealedEnvelope & se, std::array<Envelope, N> && envs) {
			for (Envelope & e : envs)
				e.AddSealedEnvelope(se);
			Resources::m_queue.Push(std::forward<decltype(envs)>(envs));
			for (Envelope & e : envs)
				e.OpenSealedEnvelope();
		}

		//Queues a vector of envelopes
		void Push(SealedEnvelope & se, std::vector<Envelope> && envs);

		//Attempts to grab an envelope from the queue
		void Pop(Envelope &e);

		template<unsigned int N>
		void Call(std::array<Envelope, N> && envs, std::vector<Envelope> && batchEnvs) {
			if (envs.size() == 0 && batchEnvs.size() == 0)
				throw std::runtime_error("Attempting to call no Envelopes");

			PVOID currentFiber = GetCurrentFiber();
			auto completionJob = [=]() {
				Nova::Push(MakeJob(&FinishCalledJob, currentFiber));
			};

			SealedEnvelope se(Envelope{ &completionJob });
			Resources::m_callTrigger = &se;

			Push(se, std::forward<decltype(envs)>(envs));

			Push(se, std::forward<decltype(batchEnvs)>(batchEnvs));

			LPVOID newFiber;

			if (Resources::m_availableFibers.size() > 0) {
				newFiber = Resources::m_availableFibers[Resources::m_availableFibers.size() - 1];
				Resources::m_availableFibers.pop_back();
			}
			else
				newFiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)OpenCallTriggerAndEnterJobLoop, nullptr);

			SwitchToFiber(newFiber);
		}

		void FinishCalledJob(LPVOID);

		//Starting point for a new fiber, queues a list of jobs and immediately enters the job loop.
		//This is used by the Call- functions to delay queueing of jobs until after the calling fiber
		//has been suspended.
		void OpenCallTriggerAndEnterJobLoop(LPVOID jobPtr);
	}
}
