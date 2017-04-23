#pragma once

#include <deque>
#include <vector>
#include <Windows.h>
#include "Envelope.h"
#include "Globals.h"
#include "Job.h"
#include "blockingconcurrentqueue.h"
#include "WorkerThread.h"

namespace Nova {

	class Queue {
	public:
		//Builds and queues a standalone job
		template<typename Callable, typename ... Params>
		static void Push(Callable callable, Params... args) {
			Push(MakeJob(callable, args...));
		}

		//Queues a pre-built standalone job
		template<typename Callable, typename ... Params>
		static void Push(SimpleJob<Callable, Params...> & j) {
			auto* basePtr = new SimpleJob<Callable, Params...>(j);
			Envelope env(&Envelope::RunAndDeleteRunnable<SimpleJob<Callable, Params...>>, basePtr);
			PushEnvelopes(env);
		}

		//Queues a pre-built standalone job (rvalue)
		template<typename Callable, typename ... Params>
		static void Push(SimpleJob<Callable, Params...> && j) {
			Push(j);
		}

		//Builds and queues a batch job
		template<typename Callable, typename ... Params>
		static void PushBatch(Callable callable, Params... args) {
			Push(MakeBatchJob(callable, args...));
		}

		//Queues a pre-built batch job
		template<typename Callable, typename ... Params>
		static void Push(BatchJob<Callable, Params...> & j) {
			PushEnvelopes(SplitBatchJob(j));
		}

		//Queues a pre-built batch job with a custom sealed envelope
		template<typename Callable, typename ... Params>
		static void Push(BatchJob<Callable, Params...> & j, SealedEnvelope next) {
			std::vector<Envelope> jobs = SplitBatchJob(j);

			for (unsigned c = 0; c < jobs.size(); c++)
				jobs[c].AddSealedEnvelopes(next);

			PushEnvelopes(jobs);
		}

		//Queues a pre-built batch job (rvalue)
		template<typename Callable, typename ... Params>
		static void Push(BatchJob<Callable, Params...> && j) {
			Push(j);
		}

		//Queues a pre-built batch job (rvalue) with a custom sealed envelope
		template<typename Callable, typename ... Params>
		static void Push(BatchJob<Callable, Params...> && j, SealedEnvelope next) {
			Push(j, next);
		}

		//Queues a set of Runnables
		template<typename ... Runnables>
		static void Push(Runnables&... runnables) {
			std::vector<Envelope> envs;
			PackRunnable(envs, runnables...);
			PushEnvelopes(envs);
		}

		//Queues a set of Runnables (rvalues)
		template<typename ... Runnables>
		static void Push(Runnables&&... runnables) {
			Push(runnables...);
		}

		//Queues a set of Runnables
		template<typename ... Runnables>
		static void Push(Runnables&... runnables, SealedEnvelope se) {
			std::vector<Envelope> envs;
			PackRunnable(envs, runnables...);
			for (unsigned c = 0; c < envs.size(); c++)
				envs[c].AddSealedEnvelopes(se);
			PushEnvelopes(envs);
		}

		//Queues a set of Runnables (rvalues)
		template<typename ... Runnables>
		static void Push(Runnables&&... runnables, SealedEnvelope se) {
			Push(runnables..., se);
		}

		//Attempts to grab an envelope from the calling thread's queue. Returns true if successful
		static void PopJob(Envelope &e);

		//Loads a set of runnables, queues them across the pool, then pauses the current call stack until they finish
		template<typename ... Runnables>
		static void Call(Runnables&... runnables) {
			std::vector<Envelope> envs;
			PackRunnableNoAlloc(envs, runnables...);
			Call(envs);
		}

		template<typename Callable, typename ... Params>
		static void ParallelFor(Callable callable, unsigned start, unsigned end, Params... args) {
			Call(MakeBatchJob([&](BatchIndex start, BatchIndex end, Params... args) {
				for (BatchIndex c = start; c < end; c++)
					callable(c, args...);
			}, start, end, args...));
		}

	private:
		//Breaks a runnable off the parameter pack and recurses
		template<typename Runnable, typename ... Runnables>
		static void PackRunnable(std::vector<Envelope> & envs, Runnable & runnable, Runnables&... runnables) {
			PackRunnable(envs, runnable);
			PackRunnable(envs, runnables...);
		}

		//Loads a runnable into an envelope and pushes it to the given vector
		template<typename Runnable>
		static void PackRunnable(std::vector<Envelope> & envs, Runnable& runnable) {
			auto* basePtr = new Runnable(runnable);
			envs.push_back({ &Envelope::RunAndDeleteRunnable<Runnable>, basePtr });
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector
		template<typename Callable, typename ... Params>
		static void PackRunnable(std::vector<Envelope> & envs, BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> splitEnvs = SplitBatchJob(j);
			envs.insert(envs.end(), splitEnvs.begin(), splitEnvs.end());
		}

		//Breaks a runnable off the parameter pack and recurses
		template<typename Runnable, typename ... Runnables>
		static void PackRunnableNoAlloc(std::vector<Envelope> & envs, Runnable & runnable, Runnables&... runnables) {
			PackRunnableNoAlloc(envs, runnable);
			PackRunnableNoAlloc(envs, runnables...);
		}

		//Loads a runnable into an envelope and pushes it to the given vector
		template<typename Runnable>
		static void PackRunnableNoAlloc(std::vector<Envelope> & envs, Runnable& runnable) {
			envs.push_back({ &Envelope::RunRunnable<Runnable>, &runnable });
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector
		template<typename Callable, typename ... Params>
		static void PackRunnableNoAlloc(std::vector<Envelope> & envs, BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> splitEnvs = SplitBatchJobNoAlloc(j);
			envs.insert(envs.end(), splitEnvs.begin(), splitEnvs.end());
		}

		//Converts a BatchJob into a vector of Envelopes
		template<typename Callable, typename ... Params>
		static std::vector<Envelope> SplitBatchJob(BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> jobs(j.GetSections());
			auto* basePtr = new BatchJob<Callable, Params...>(j);
			SealedEnvelope se(Envelope(&Envelope::DeleteRunnable<BatchJob<Callable, Params...>>, basePtr));
			for (unsigned int section = 1; section <= j.GetSections(); section++) {
				Envelope gj(basePtr);
				gj.AddSealedEnvelopes(se);
				jobs.push_back(gj);
			}
			return jobs;
		}

		//Converts a BatchJob into a vector of Envelopes
		template<typename Callable, typename ... Params>
		static std::vector<Envelope> SplitBatchJobNoAlloc(BatchJob<Callable, Params...> & j) {
			std::vector<Envelope> jobs(j.GetSections());
			for (unsigned int section = 1; section <= j.GetSections(); section++) {
				Envelope gj(&j);
				jobs.push_back(gj);
			}
			return jobs;
		}

		//Pushes an envelope to the calling thread's neighbor's queue
		static void Queue::PushEnvelopes(Envelope& e);

		//Pushes an envelope to the calling thread's neighbor's queue
		static void Queue::PushEnvelopes(Envelope&& e);

		//Pushes a vector of envelopes across the pool
		static void Queue::PushEnvelopes(std::vector<Envelope> & envs);

		static void Queue::Call(std::vector<Envelope> & e);

		static void Queue::FinishCalledJob(LPVOID);

		//Starting point for a new fiber, queues a list of jobs and immediately enters the job loop.
		//This is used by the Call- functions to delay queueing of jobs until after the calling fiber
		//has been suspended.
		static void Queue::QueueJobsAndEnterJobLoop(LPVOID jobPtr);

		static moodycamel::BlockingConcurrentQueue<Envelope> m_queue;
		static thread_local std::vector<LPVOID> m_availableFibers;
		static thread_local std::vector<Envelope> m_currentJobs;
	};
}
