#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

template <std::size_t S>
struct fd_queue
{
	static constexpr std::size_t SIZE = S;

	struct slot
	{
		alignas(64) std::atomic<std::uint32_t> lock;
		int data;
	};

	alignas(64) std::atomic<std::uint32_t> head;
	alignas(64) std::atomic<std::uint32_t> tail;
	alignas(64) std::unique_ptr<slot[]> slots;

	fd_queue() : head(0), tail(0), slots(new slot[SIZE])
	{
		for (std::size_t i = 0; i < SIZE; i++)
			slots[i].lock.store(i, std::memory_order_relaxed);
	}

	std::uint64_t push(int data)
	{
		const std::size_t n = tail.fetch_add(1, std::memory_order_relaxed);
		slot &s = slots[n & (SIZE - 1)];

		std::uint64_t overflows = 0;
		while (s.lock.load(std::memory_order_acquire) != n) {
			++overflows;
			__builtin_ia32_pause();
		}

		s.data = data;
		s.lock.store(n + 1, std::memory_order_release);

		return overflows;
	}

	int pop()
	{
		const std::size_t n = head.fetch_add(1, std::memory_order_relaxed);
		slot &s = slots[n & (SIZE - 1)];

		while (s.lock.load(std::memory_order_acquire) != (n + 1))
			__builtin_ia32_pause();

		int data = s.data;
		s.lock.store(n + SIZE, std::memory_order_release);

		return data;
	}
};
