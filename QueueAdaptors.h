#pragma once

#include "concurrentqueue.h"

template<typename T>
class MoodycamelAdaptor {
public:
	bool Pop(T& item) {
		return m_queue.try_dequeue(item);
	}

	void Push(T& item) {
		m_queue.enqueue(std::move(item));
	}

	template<unsigned N>
	void Push(std::array<T, N> && items) {
		m_queue.enqueue_bulk(std::make_move_iterator(std::begin(items)), items.size());
	}

	void Push(std::vector<T> && items) {
		m_queue.enqueue_bulk(std::make_move_iterator(std::begin(items)), items.size());
	}
private:
	moodycamel::ConcurrentQueue<T> m_queue;
};

#define NOVA_QUEUE_TYPE MoodycamelAdaptor