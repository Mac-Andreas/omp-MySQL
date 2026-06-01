/* =========================================
 *
 *  MySQL for open.mp  —  async query engine
 *  ------------------------------------------
 *
 *  Per-connection worker model: each Connection owns exactly one worker thread
 *  and an input queue, so a given MYSQL* is only ever touched by one thread —
 *  no locking around libmysqlclient, no data races. Finished jobs go to a
 *  shared completed-queue that the main thread drains once per tick to fire
 *  Pawn callbacks.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 *
 * ========================================= */

#pragma once

#include "Query.hpp"
#include "HashJob.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Connection;

// Drives one Connection's queries on a dedicated thread. The Connection must
// outlive its worker; stop() is called before the Connection is destroyed.
class ConnectionWorker
{
public:
	// `completed` / `completedMutex` are shared sinks owned by the engine.
	ConnectionWorker(Connection* conn,
		std::deque<std::unique_ptr<QueryJob>>* completed,
		std::mutex* completedMutex);
	~ConnectionWorker();

	ConnectionWorker(const ConnectionWorker&) = delete;
	ConnectionWorker& operator=(const ConnectionWorker&) = delete;

	void enqueue(std::unique_ptr<QueryJob> job);
	void stop();

	int pending() const { return pending_.load(); }

private:
	void loop();

	Connection* conn_;
	std::deque<std::unique_ptr<QueryJob>>* completed_;
	std::mutex* completedMutex_;

	std::thread thread_;
	std::atomic<bool> running_ { true };
	std::atomic<int> pending_ { 0 };

	std::deque<std::unique_ptr<QueryJob>> input_;
	std::mutex inputMutex_;
	std::condition_variable inputCv_;
};

// The engine: a shared query completed-queue plus a small hashing thread pool.
// Both are drained on the main tick.
class QueryEngine
{
public:
	~QueryEngine();

	/// Move all finished query jobs out for main-thread callback dispatch.
	std::vector<std::unique_ptr<QueryJob>> drainCompleted();

	std::deque<std::unique_ptr<QueryJob>>* completedSink() { return &completed_; }
	std::mutex* completedMutex() { return &completedMutex_; }

	// --- hashing ---
	/// Start the hashing workers (idempotent).
	void startHashing(unsigned int threads = 2);
	/// Enqueue a hash/verify job.
	void enqueueHash(std::unique_ptr<HashJob> job);
	/// Move all finished hash jobs out for main-thread dispatch.
	std::vector<std::unique_ptr<HashJob>> drainHashes();

private:
	void hashLoop();
	void stopHashing();

	std::deque<std::unique_ptr<QueryJob>> completed_;
	std::mutex completedMutex_;

	std::vector<std::thread> hashWorkers_;
	std::atomic<bool> hashRunning_ { false };
	std::deque<std::unique_ptr<HashJob>> hashInput_;
	std::mutex hashInputMutex_;
	std::condition_variable hashInputCv_;
	std::deque<std::unique_ptr<HashJob>> hashOutput_;
	std::mutex hashOutputMutex_;
};
