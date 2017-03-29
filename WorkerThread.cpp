#include "WorkerThread.h"
#include "GenericJob.h"
#include "JobQueue.h"
#include "Job.h"

thread_local unsigned int WorkerThread::s_thread_id = 0;
unsigned int WorkerThread::s_threadCount = 1;
std::mutex WorkerThread::s_countLock;

WorkerThread::WorkerThread() {
	m_thread = std::thread(InitThread);
}

void WorkerThread::JobLoop() {
	
	while (true) {
		GenericJob j;
		if (JobQueuePool::PopJob(j)) {
			j.m_task(j.m_data);
		}
	}
}

unsigned int WorkerThread::GetThreadId() {
	return s_thread_id;
}

unsigned int WorkerThread::GetThreadCount()
{
	return s_threadCount;
}

void WorkerThread::InitThread()
{
	{
		lock_guard<mutex> lock(s_countLock);
		s_thread_id = s_threadCount;
		s_threadCount++;
	}
	ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
	JobLoop();
}
