/* =========================================
 *
 *  MySQL for open.mp  —  async query engine impl
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#include "QueryQueue.hpp"
#include "Connection.hpp"
#include "Statement.hpp"
#include "Hashing.hpp"

#include <chrono>

// --- ConnectionWorker ------------------------------------------------------

ConnectionWorker::ConnectionWorker(Connection* conn,
	std::deque<std::unique_ptr<QueryJob>>* completed,
	std::mutex* completedMutex)
	: conn_(conn)
	, completed_(completed)
	, completedMutex_(completedMutex)
{
	thread_ = std::thread([this] { loop(); });
}

ConnectionWorker::~ConnectionWorker()
{
	stop();
}

void ConnectionWorker::stop()
{
	if (!running_.exchange(false))
	{
		return;
	}
	inputCv_.notify_all();
	if (thread_.joinable())
	{
		thread_.join();
	}
	std::lock_guard<std::mutex> lk(inputMutex_);
	input_.clear();
	pending_.store(0);
}

void ConnectionWorker::enqueue(std::unique_ptr<QueryJob> job)
{
	{
		std::lock_guard<std::mutex> lk(inputMutex_);
		input_.push_back(std::move(job));
	}
	pending_.fetch_add(1);
	inputCv_.notify_one();
}

void ConnectionWorker::loop()
{
	while (true)
	{
		std::unique_ptr<QueryJob> job;
		{
			std::unique_lock<std::mutex> lk(inputMutex_);
			inputCv_.wait(lk, [this] { return !running_.load() || !input_.empty(); });
			// On shutdown, still DRAIN any queued jobs before exiting. This matters
			// for statement-teardown jobs: their mysql_stmt_close must run on this
			// worker thread, never on the main thread that called stop().
			if (input_.empty())
			{
				if (!running_.load())
				{
					return;
				}
				continue;
			}
			job = std::move(input_.front());
			input_.pop_front();
		}

		// Statement teardown: destroy the owned PreparedStatement HERE, on the
		// thread that owns conn_->raw(), so mysql_stmt_close never runs on the main
		// thread. No result, no callback — just drop it and move on.
		if (job->statementToClose != nullptr)
		{
			job->statementToClose.reset();
			pending_.fetch_sub(1);
			continue;
		}

		// Only this thread ever touches conn_, so no locking is needed around
		// the libmysqlclient calls. Results are fully buffered here (off the
		// main thread) and carried back in the job.
		if (conn_ == nullptr)
		{
			job->succeeded = false;
			job->error = "invalid connection";
		}
		else if (job->statement != nullptr)
		{
			// Prepared-statement execute path.
			auto result = std::make_unique<QueryResult>();
			result->queryString = job->sql;
			auto start = std::chrono::steady_clock::now();
			job->succeeded = job->statement->execute(*result);
			auto end = std::chrono::steady_clock::now();
			result->execTimeMicros
				= std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
			job->errorCode = job->statement->lastErrno();
			job->error = job->succeeded ? std::string() : job->statement->lastError();
			if (job->succeeded)
			{
				job->result = std::move(result);
			}
		}
		else
		{
			auto result = std::make_unique<QueryResult>();
			auto start = std::chrono::steady_clock::now();
			job->succeeded = conn_->queryBuffered(job->sql, *result);
			auto end = std::chrono::steady_clock::now();
			result->execTimeMicros
				= std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
			job->errorCode = conn_->lastErrno();
			job->error = job->succeeded ? std::string() : conn_->lastError();
			if (job->succeeded)
			{
				job->result = std::move(result);
			}
		}

		{
			std::lock_guard<std::mutex> lk(*completedMutex_);
			completed_->push_back(std::move(job));
		}
		pending_.fetch_sub(1);
	}
}

// --- QueryEngine -----------------------------------------------------------

QueryEngine::~QueryEngine()
{
	stopHashing();
}

std::vector<std::unique_ptr<QueryJob>> QueryEngine::drainCompleted()
{
	std::vector<std::unique_ptr<QueryJob>> out;
	std::lock_guard<std::mutex> lk(completedMutex_);
	out.reserve(completed_.size());
	while (!completed_.empty())
	{
		out.push_back(std::move(completed_.front()));
		completed_.pop_front();
	}
	return out;
}

void QueryEngine::startHashing(unsigned int threads)
{
	if (hashRunning_.load())
	{
		return;
	}
	hashRunning_.store(true);
	if (threads < 1)
	{
		threads = 1;
	}
	for (unsigned int i = 0; i < threads; ++i)
	{
		hashWorkers_.emplace_back([this] { hashLoop(); });
	}
}

void QueryEngine::stopHashing()
{
	if (!hashRunning_.exchange(false))
	{
		return;
	}
	hashInputCv_.notify_all();
	for (std::thread& t : hashWorkers_)
	{
		if (t.joinable())
		{
			t.join();
		}
	}
	hashWorkers_.clear();
}

void QueryEngine::enqueueHash(std::unique_ptr<HashJob> job)
{
	{
		std::lock_guard<std::mutex> lk(hashInputMutex_);
		hashInput_.push_back(std::move(job));
	}
	hashInputCv_.notify_one();
}

std::vector<std::unique_ptr<HashJob>> QueryEngine::drainHashes()
{
	std::vector<std::unique_ptr<HashJob>> out;
	std::lock_guard<std::mutex> lk(hashOutputMutex_);
	out.reserve(hashOutput_.size());
	while (!hashOutput_.empty())
	{
		out.push_back(std::move(hashOutput_.front()));
		hashOutput_.pop_front();
	}
	return out;
}

void QueryEngine::hashLoop()
{
	while (true)
	{
		std::unique_ptr<HashJob> job;
		{
			std::unique_lock<std::mutex> lk(hashInputMutex_);
			hashInputCv_.wait(lk, [this] { return !hashRunning_.load() || !hashInput_.empty(); });
			if (!hashRunning_.load() && hashInput_.empty())
			{
				return;
			}
			if (hashInput_.empty())
			{
				continue;
			}
			job = std::move(hashInput_.front());
			hashInput_.pop_front();
		}

		if (job->op == HashJob::Op::Hash)
		{
			job->result = Hashing::hashArgon2id(job->password);
		}
		else
		{
			job->verified = Hashing::verify(job->password, job->hash);
		}

		{
			std::lock_guard<std::mutex> lk(hashOutputMutex_);
			hashOutput_.push_back(std::move(job));
		}
	}
}
