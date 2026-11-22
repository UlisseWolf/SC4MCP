// SC4MCP - runs work on the game thread.
#include "MainThreadQueue.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace
{
	struct Job
	{
		std::function<std::string()> work;
		std::string result;
		bool done = false;
		bool abandoned = false;
	};

	std::mutex queueMutex;
	std::condition_variable jobDone;
	std::deque<std::shared_ptr<Job>> pending;
	bool closed = false;
}

bool MainThreadQueue::Run(const std::function<std::string()>& job, std::string& result, std::chrono::milliseconds timeout)
{
	auto entry = std::make_shared<Job>();
	entry->work = job;
	std::unique_lock<std::mutex> lock(queueMutex);
	if (closed) return false;
	pending.push_back(entry);
	if (!jobDone.wait_for(lock, timeout, [&] { return entry->done || closed; }) || !entry->done)
	{
		entry->abandoned = true; // the game thread skips it if it has not started yet
		return false;
	}
	result = std::move(entry->result);
	return true;
}

void MainThreadQueue::RunPending()
{
	std::deque<std::shared_ptr<Job>> batch;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		batch.swap(pending);
	}
	for (const std::shared_ptr<Job>& job : batch)
	{
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			if (job->abandoned) continue;
		}
		std::string result;
		try
		{
			result = job->work();
		}
		catch (...)
		{
			result = "{\"error\":\"exception on the game thread\"}";
		}
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			job->result = std::move(result);
			job->done = true;
		}
		jobDone.notify_all();
	}
}

void MainThreadQueue::Close()
{
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		closed = true;
		pending.clear();
	}
	jobDone.notify_all();
}
