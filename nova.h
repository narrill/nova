#pragma once

#include <vector>
#include <array>
#include <thread>
#include <mutex>
#include <tuple>
#include <Windows.h>
#include <memory>

#include "queue_adaptors.h"

#define NOVA_CACHE_LINE_BYTES 64

namespace nova {

#pragma region function & batch_function

	namespace impl{

#pragma region helpers

		template <class F, class... Args>
		inline auto invoke(F&& f, Args&&... args) ->
			decltype(std::forward<F>(f)(std::forward<Args>(args)...)) {
			return std::forward<F>(f)(std::forward<Args>(args)...);
		}

		template <class Base, class T, class Derived>
		inline auto invoke(T Base::*pmd, Derived&& ref) ->
			decltype(std::forward<Derived>(ref).*pmd) {
			return std::forward<Derived>(ref).*pmd;
		}

		namespace detail {
			template <class F, class Tuple, std::size_t... I>
			constexpr decltype(auto) apply_impl(F &&f, Tuple &&t, std::index_sequence<I...>) {
				return impl::invoke(std::forward<F>(f), std::get<I>(std::forward<Tuple>(t))...);
			}
		}  // namespace detail

		template <class F, class Tuple>
		constexpr decltype(auto) apply(F &&f, Tuple &&t) {
			return detail::apply_impl(
				std::forward<F>(f), std::forward<Tuple>(t),
				std::make_index_sequence<std::tuple_size<std::decay_t<Tuple>>::value>{});
		}

		template<class Tuple>
		struct integral_index;

		template<bool Val, class Tuple>
		struct integral_index_inner;

		template<class T, class ... Types>
		struct integral_index<std::tuple<T, Types...>> {
			static const std::size_t value = integral_index_inner<std::is_integral<T>::value, std::tuple<T, Types...>>::value;
		};

		template<typename Tuple>
		struct integral_index_inner<true, Tuple> {
			static const std::size_t value = 0;
		};

		template<typename T, typename ... Types>
		struct integral_index_inner<false, std::tuple<T, Types...>> {
			static const std::size_t value = 1 + integral_index<std::tuple<Types...>>::value;
		};

#pragma endregion

		template<typename Callable, typename ... Params>
		class batch_function;

		template <typename Callable, typename ... Params>
		class function {
		public:
			template<typename _Callable, typename ... _Params, std::enable_if_t<!std::is_same<std::decay_t<_Callable>, function<Callable, Params...>>::value, int> = 0>
			function(_Callable&& callable, _Params&&... args)
				: m_callable(std::forward<_Callable>(callable)), m_tuple(std::forward<_Params>(args)...) {
			}

			void operator () () {
				impl::apply(m_callable, m_tuple);
			}

			//Ignore the squiggly, this is defined further down
			//operator BatchJob<Callable, Params...>() const;

			typedef batch_function<Callable, Params...> batchType;

		protected:
			typedef std::tuple<Params...> tuple_t;
			Callable m_callable;
			tuple_t m_tuple;
		};

		template <typename Callable, typename ... Params>
		class batch_function : public function<Callable, Params...> {
		public:
			typedef impl::integral_index<tuple_t> tupleIntegralIndex;
			typedef std::tuple_element_t<tupleIntegralIndex::value, tuple_t> start_index_t;
			typedef std::tuple_element_t<tupleIntegralIndex::value + 1, tuple_t> end_index_t;

			template<typename _Callable, typename ... _Params, std::enable_if_t<!std::is_same<std::decay_t<_Callable>, batch_function<Callable, Params...>>::value, int> = 0>
			batch_function(_Callable&& callable, _Params&&... args)
				: function<Callable, Params...>(std::forward<_Callable>(callable), std::forward<_Params>(args)...), m_sections((std::min)(static_cast<std::size_t>(end() - start()), impl::worker_thread::get_thread_count())) {
			}

			/*explicit BatchJob(SimpleJob<Callable, Params...> & sj)
			: SimpleJob<Callable, Params...>(sj), m_sections((std::min)(End() - Start(), static_cast<indexType>(internal::WorkerThread::GetThreadCount()))) {
			}*/

			void operator () () {
				tuple_t params = this->m_tuple;
				std::size_t batchStart = start();
				std::size_t batchEnd = end();
				float count = static_cast<float>(batchEnd - batchStart);
				std::size_t section = static_cast<start_index_t>(InterlockedIncrement(&m_currentSection));
				std::size_t newStart = static_cast<start_index_t>(batchStart + floorf(static_cast<float>(count*(section - 1) / m_sections)));
				batchEnd = static_cast<std::size_t>(batchStart + floorf(count*section / m_sections));

				start(params) = static_cast<start_index_t>(newStart);
				end(params) = static_cast<end_index_t>(batchEnd);
				impl::apply(this->m_callable, params);
			}

			std::size_t get_sections() const {
				return m_sections;
			}

			void* operator new(size_t i)
			{
				return _mm_malloc(i, 32);
			}

			void operator delete(void* p)
			{
				_mm_free(p);
			}

			typedef function<Callable, Params...> simpleType;

		private:
			typedef simpleType::tuple_t tuple_t;
			alignas(32) uint32_t m_currentSection = 0;
			std::size_t m_sections;

			start_index_t& start() {
				return start(this->m_tuple);
			}
			static start_index_t& start(tuple_t & tuple) {
				return std::get<tupleIntegralIndex::value>(tuple);
			}
			end_index_t& end() {
				return end(this->m_tuple);
			}
			static end_index_t& end(tuple_t & tuple) {
				return std::get<tupleIntegralIndex::value + 1>(tuple);
			}
		};

		/*template<typename Callable, typename ...Params>
		inline SimpleJob<Callable, Params...>::operator BatchJob<Callable, Params...>() const {
		return BatchJob<Callable, Params...>(*this);
		}*/
	}

	// Returns a Runnable wrapper for the given Callable and parameters.
	template <typename Callable, typename ... Params>
	auto bind(Callable&& callable, Params&&... args) {
		return impl::function<std::decay_t<Callable>, std::decay_t<Params>...>(std::forward<Callable>(callable), std::forward<Params>(args)...);
	}

	// Returns a Batch Runnable wrapper for the given Callable and parameters. The Callable must have two sequential integral parameters; these are, respectively, the start and end of the range over which the batch will be split.
	template <typename Callable, typename ... Params>
	auto bind_batch(Callable&& callable, Params&&... args) {
		return impl::batch_function<std::decay_t<Callable>, std::decay_t<Params>...>(std::forward<Callable>(callable), std::forward<Params>(args)...);
	}

#pragma endregion

#pragma region job & dependency_token

#pragma region helpers

	namespace impl{

		template<typename T>
		struct is_shared {
			static const bool value = false;
		};

		template<typename T>
		struct is_shared<std::shared_ptr<T>> {
			static const bool value = true;
		};
	}

#pragma endregion

	namespace impl{ class job; }

	// Takes a Runnable and invokes it when all copies of the token are released or destroyed.
	class dependency_token {
	public:
		dependency_token() {}

		dependency_token(impl::job & e);

		dependency_token(impl::job && e);

		dependency_token(const dependency_token &) = default;

		dependency_token& operator=(const dependency_token&) = default;

		dependency_token(dependency_token &&) = default;

		dependency_token& operator=(dependency_token&&) = default;

		template<typename Runnable, std::enable_if_t<!std::is_same<std::decay_t<Runnable>, dependency_token>::value, int> = 0>
		dependency_token(Runnable&& runnable);

		// Releases the token.
		void Open() {
			m_token.reset();
		}
	private:
		struct shared_token;
		std::shared_ptr<shared_token> m_token;
	};

	namespace impl{

		template<typename T>
		constexpr std::size_t ceil(T num)
		{
			return (static_cast<T>(static_cast<std::size_t>(num)) == num)
				? static_cast<std::size_t>(num)
				: static_cast<std::size_t>(num) + ((num > 0) ? 1 : 0);
		}

		template<typename T>
		class raii_ptr {
		public:
			raii_ptr(std::decay_t<T> *& ptr, std::decay_t<T> * val)
				: m_ptr(ptr) {
				m_ptr = val;
			}
			~raii_ptr() {
				m_ptr = nullptr;
			}
		private:
			T *& m_ptr;
		};

		constexpr std::size_t padSize = static_cast<std::size_t>(ceil(NOVA_CACHE_LINE_BYTES));

		class alignas(NOVA_CACHE_LINE_BYTES) job {
		public:
			job() {}
			~job() {
				m_data.m_deleteFunc(m_data.m_runnable);
			}
			job(const job &) = delete;
			job operator=(const job &) = delete;
			job(job && e) noexcept {
				move(std::forward<job>(e));
			}
			job& operator=(job && e) noexcept {
				m_data.m_deleteFunc(m_data.m_runnable);
				move(std::forward<job>(e));
				return *this;
			}

			template<typename Runnable, std::enable_if_t<!std::is_same<std::decay_t<Runnable>, job>::value, int> = 0>
			job(Runnable&& runnable)
				: m_data(
					&job::run_runnable<std::decay_t<Runnable>>,
					&job::delete_runnable<std::decay_t<Runnable>>,
					new std::decay_t<Runnable>(std::forward<Runnable>(runnable))
				) {
			}

			template<typename Runnable, std::enable_if_t<!std::is_same<std::decay_t<Runnable>, job>::value, int> = 0>
			job(Runnable * runnable)
				: m_data(
					&job::run_runnable<std::decay_t<Runnable>>,
					&job::no_op,
					runnable
				) {
			}

			job(void(*func)())
				: m_data(
					&job::run_runnable<void()>,
					&job::no_op,
					func
				) {

			}

			job(void(*runFunc)(void*), void(*deleteFunc)(void*), void * runnable)
				: m_data( runFunc, deleteFunc, runnable ) {
			}

			void operator () () const {
				m_data.m_runFunc(m_data.m_runnable);
			}

			void set_dependency_token(dependency_token & se) {
				m_data.m_callToken = se;
			}

			void set_dependency_token(dependency_token && se) {
				m_data.m_callToken = std::forward<dependency_token>(se);
			}
			void open_dependency_token() {
				m_data.m_callToken.Open();
			}

			dependency_token & get_dependency_token() {
				return m_data.m_callToken;
			}

			template <typename Runnable, std::enable_if_t<!is_shared<Runnable>::value, int> = 0>
			static void run_runnable(void * runnable) {
				(*(static_cast<Runnable*>(runnable)))();
			}

			template <typename Runnable, std::enable_if_t<is_shared<Runnable>::value, int> = 0>
			static void run_runnable(void * runnable) {
				(*(static_cast<Runnable*>(runnable)->get()))();
			}

			template <typename Runnable>
			static void delete_runnable(void * runnable) {
				delete static_cast<Runnable*>(runnable);
			}

			static void no_op(void * runnable) {};

		private:
			void move(job && e) noexcept {
				m_data.m_runFunc = e.m_data.m_runFunc;
				m_data.m_deleteFunc = e.m_data.m_deleteFunc;
				m_data.m_runnable = e.m_data.m_runnable;
				m_data.m_callToken = std::move(e.m_data.m_callToken);
				e.m_data.m_runFunc = &job::no_op;
				e.m_data.m_deleteFunc = &job::no_op;
				e.m_data.m_runnable = nullptr;
			}

			struct JobData {
				JobData() {}
				JobData(void(*runFunc)(void*), void(*deleteFunc)(void*), void * runnable) 
					: m_runFunc(runFunc), m_deleteFunc(deleteFunc), m_runnable(runnable) {
				}
				void(*m_runFunc)(void *) = &job::no_op;
				void(*m_deleteFunc)(void*) = &job::no_op;
				void * m_runnable = nullptr;
				dependency_token m_callToken;
			};

			JobData m_data;
			char padding[padSize - sizeof(JobData)];
		};
	}

	struct dependency_token::shared_token {
		shared_token(impl::job & e)
			: m_job(std::move(e)) {
		}

		template <typename Runnable>
		shared_token(Runnable&& runnable)
			: m_job(std::forward<Runnable>(runnable)) {
		}

		~shared_token() {
			m_job();
		}

		impl::job m_job;
	};

	template<typename Runnable, std::enable_if_t<!std::is_same<std::decay_t<Runnable>, dependency_token>::value, int> = 0>
	dependency_token::dependency_token(Runnable&& runnable)
		: m_token(std::make_shared<shared_token>(std::forward<std::decay_t<Runnable>>(runnable))) {
	}

	inline dependency_token::dependency_token(impl::job & e)
		: m_token(std::make_shared<shared_token>(e)) {
	}

	inline dependency_token::dependency_token(impl::job && e)
		: m_token(std::make_shared<shared_token>(std::forward<impl::job>(e))) {
	}

#pragma endregion

#pragma region queue_wrapper

#define SPIN_COUNT 10000

#ifndef NOVA_QUEUE_TYPE
#define NOVA_QUEUE_TYPE MoodycamelAdaptor
#endif // !NOVA_QUEUE_TYPE

	namespace impl{
		class critical_lock {
		public:
			critical_lock(CRITICAL_SECTION& cs) : m_cs(cs) {
				EnterCriticalSection(&m_cs);
			}

			~critical_lock() {
				LeaveCriticalSection(&m_cs);
			}

		private:
			critical_lock(const critical_lock&) = delete;

		private:
			CRITICAL_SECTION & m_cs;
		};

		struct critical_wrapper {
			critical_wrapper() {
				InitializeCriticalSection(&cs);
			}

			~critical_wrapper() {
				DeleteCriticalSection(&cs);
			}

			CRITICAL_SECTION cs;
		};

		template <typename T>
		class queue_wrapper {
		public:
			queue_wrapper() {
				InitializeConditionVariable(&s_cv);
				InitializeConditionVariable(&s_mainCV);
			}

			void pop(T& item) {
				unsigned counter = 0;
				while (!m_globalQueue.pop(item)) {
					if (counter++ > SPIN_COUNT) {
						counter = 0;
						SleepConditionVariableCS(&s_cv, &s_cs.cs, INFINITE);
					}
				}
			}

			void pop_main(T& item) {
				unsigned counter = 0;
				while (!m_globalQueue.pop(item) && !m_mainQueue.pop(item)) {
					if (counter++ > SPIN_COUNT) {
						counter = 0;
						SleepConditionVariableCS(&s_mainCV, &s_cs.cs, INFINITE);
					}
				}
			}

			template<bool ToMain, std::enable_if_t<!ToMain, int> = 0>
			void push(T&& item) {
				m_globalQueue.push(std::forward<T>(item));
				WakeConditionVariable(&s_cv);
				WakeConditionVariable(&s_mainCV);
			}

			template<bool ToMain, typename Collection, std::enable_if_t<!ToMain, int> = 0>
			void push(Collection && items) {
				m_globalQueue.push(std::forward<decltype(items)>(items));
				WakeAllConditionVariable(&s_cv);
				WakeConditionVariable(&s_mainCV);
			}

			template<bool ToMain, std::enable_if_t<ToMain, int> = 0>
			void push(T&& item) {
				m_mainQueue.push(std::forward<T>(item));
				WakeConditionVariable(&s_mainCV);
			}

			template<bool ToMain, typename Collection, std::enable_if_t<ToMain, int> = 0>
			void push(Collection && items) {
				m_mainQueue.push(std::forward<decltype(items)>(items));
				WakeConditionVariable(&s_mainCV);
			}

		private:
			NOVA_QUEUE_TYPE<T> m_globalQueue;
			NOVA_QUEUE_TYPE<T> m_mainQueue;
			static CONDITION_VARIABLE s_cv;
			static CONDITION_VARIABLE s_mainCV;
			static thread_local critical_wrapper s_cs;
			static thread_local critical_lock s_cl;
		};

		template<typename T>
		CONDITION_VARIABLE queue_wrapper<T>::s_cv;

		template<typename T>
		CONDITION_VARIABLE queue_wrapper<T>::s_mainCV;

		template<typename T>
		thread_local critical_wrapper queue_wrapper<T>::s_cs;

		template<typename T>
		thread_local critical_lock queue_wrapper<T>::s_cl(queue_wrapper<T>::s_cs.cs);
	}

#pragma endregion

#pragma region resources

	namespace impl{
		class resources {
		public:
			//Meyers singletons
			static queue_wrapper<job>& queue_wrapper() {
				static nova::impl::queue_wrapper<nova::impl::job> qw;
				return qw;
			}
			static std::vector<LPVOID>& available_fibers() {
				static thread_local std::vector<LPVOID> af;
				return af;
			}
			static dependency_token *& call_token() {
				static thread_local dependency_token * ct;
				return ct;
			}
			static dependency_token *& dependent_token() {
				static thread_local dependency_token * se;
				return se;
			}
		};
	}

#pragma endregion

#pragma region worker_thread

	namespace impl {
		//Forward declarations
		void pop_main(job &);
		void pop(job &);

		class worker_thread {
		public:
			worker_thread() {
				m_thread = std::thread(init_thread);
			}
			static void job_loop() {
				while (running()) {
					job j;
					if (worker_thread::get_thread_id() == 0)
						pop_main(j);
					else
						pop(j);

					raii_ptr<dependency_token> rp(resources::dependent_token(), &j.get_dependency_token());
					j();
				}
			}
			static std::size_t get_thread_id() {
				return thread_id();
			}
			static std::size_t get_thread_count() {
				return thread_count();
			}
			static void kill_worker() {
				running() = false;
			}
			void Join() {
				m_thread.join();
			}
		private:
			static void init_thread() {
				{
					std::lock_guard<std::mutex> lock(init_lock());
					thread_id() = thread_count();
					thread_count()++;
				}
				ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
				job_loop();
			}

			//Meyers singletons
			static std::size_t & thread_id() {
				static thread_local std::size_t id = 0;
				return id;
			}
			static bool & running() {
				static thread_local bool running = true;
				return running;
			}
			static std::size_t & thread_count() {
				static std::size_t count = 1;
				return count;
			}
			static std::mutex & init_lock() {
				static std::mutex lock;
				return lock;
			}

			std::thread m_thread;
		};
	}

#pragma endregion

#pragma region push

#pragma region helpers

	namespace impl{

		template<typename... Args> struct batch_count;

		template<>
		struct batch_count<> {
			static const int value = 0;
		};

		template<typename ... Params, typename... Args>
		struct batch_count<batch_function<Params...>, Args...> {
			static const int value = 1 + batch_count<Args...>::value;
		};

		template<typename First, typename... Args>
		struct batch_count<First, Args...> {
			static const int value = batch_count<Args...>::value;
		};
	}

#pragma endregion

	namespace impl{

		//Queues a set of Runnables
		template<bool ToMain, bool Dependent, typename ... Runnables >
		void push(Runnables&&... runnables) {
			using namespace impl;
			std::array<job, sizeof...(Runnables)-batch_count<Runnables...>::value> jobs;
			std::vector<job> batchJobs;
			batchJobs.reserve(batch_count<Runnables...>::value * 4);
			pack_runnable<true>(jobs, batchJobs, std::forward<Runnables>(runnables)...);
			push_picker<ToMain, Dependent>(std::move(jobs));
			push_picker<ToMain, Dependent>(std::move(batchJobs));
		}

		template<bool ToMain, bool Dependent, typename Collection, std::enable_if_t<Dependent, int> = 0>
		void push_picker(Collection && collection) {
			impl::push<ToMain>(*resources::dependent_token(), std::forward<Collection>(collection));
		}

		template<bool ToMain, bool Dependent, typename Collection, std::enable_if_t<!Dependent, int> = 0>
		void push_picker(Collection && collection) {
			impl::push<ToMain>(std::forward<Collection>(collection));
		}

		//Queues an array of Envelopes
		template<bool ToMain, std::size_t N>
		void push(std::array<impl::job, N> && jobs) {
			impl::resources::queue_wrapper().push<ToMain>(std::forward<decltype(jobs)>(jobs));
		}

		template<bool ToMain, std::size_t N>
		void push(dependency_token & dt, std::array<job, N> && jobs) {
			for (job & j : jobs)
				j.set_dependency_token(dt);
			push<ToMain>(std::forward<decltype(jobs)>(jobs));
		}

		//Queues a vector of envelopes
		template<bool ToMain>
		void push(std::vector<impl::job> && jobs) {
			impl::resources::queue_wrapper().push<ToMain>(std::forward<decltype(jobs)>(jobs));
		}

		//Queues a vector of envelopes
		template<bool ToMain>
		void push(dependency_token & dt, std::vector<job> && jobs) {
			for (job & j : jobs)
				j.set_dependency_token(dt);
			push<ToMain>(std::forward<decltype(jobs)>(jobs));
		}
	}

#pragma endregion

#pragma region call
	namespace impl{
		template<bool ToMain, bool FromMain, typename ... Params>
		void call(Params&&... params) {
			PVOID currentFiber = GetCurrentFiber();
			auto completionJob = [=]() {
				nova::push<FromMain>(bind(&finish_called_job, currentFiber));
			};

			dependency_token dt(job{ &completionJob });
			resources::call_token() = &dt;

			call_push<ToMain>(dt, std::forward<Params>(params)...);

			SwitchToFiber(get_fresh_fiber());
		}

		template<bool ToMain, std::size_t N>
		void call_push(dependency_token & dt, std::array<job, N> && jobs, std::vector<job> && batchJobs) {
			push<ToMain>(dt, std::forward<decltype(jobs)>(jobs));
			push<ToMain>(dt, std::forward<decltype(batchJobs)>(batchJobs));
		}

		template<bool ToMain>
		void call_push(dependency_token & dt) {}

		inline void finish_called_job(LPVOID oldFiber) {
			//Mark for re-use
			resources::available_fibers().push_back(GetCurrentFiber());
			SwitchToFiber(oldFiber);

			//Re-use starts here
			resources::call_token()->Open();
		}

		//Starting point for a new fiber, queues a list of jobs and immediately enters the job loop.
		//This is used by the Call- functions to delay queueing of jobs until after the calling fiber
		//has been suspended.
		inline void open_call_token_enter_job_loop(LPVOID jobPtr) {
			resources::call_token()->Open();
			worker_thread::job_loop();
		}

		inline LPVOID get_fresh_fiber() {
			LPVOID newFiber;

			if (resources::available_fibers().size() > 0) {
				newFiber = resources::available_fibers()[resources::available_fibers().size() - 1];
				resources::available_fibers().pop_back();
			}
			else
				newFiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)open_call_token_enter_job_loop, nullptr);

			return newFiber;
		}

	}

#pragma endregion

#pragma region pop

	namespace impl{

		//Attempts to grab an envelope from the queue
		inline void pop(job &j) {
			resources::queue_wrapper().pop(j);
		}

		inline void pop_main(job &j) {
			resources::queue_wrapper().pop_main(j);
		}

	}

#pragma endregion

#pragma region pack_runnable

	namespace impl{

		//Generates Envelopes from the given Runnables and inserts them into a std::array (for standalone) or a std::vector (for batch)
		template<bool Alloc, std::size_t N, typename ... Runnables>
		static void pack_runnable(std::array<job, N> & jobs, std::vector<job> & batchJobs, Runnables&&... runnables) {
			std::size_t index(0);
			pack_runnable<Alloc>(jobs, index, batchJobs, std::forward<Runnables>(runnables)...);
		}

		//Breaks a Runnable off the parameter pack and recurses
		template<bool Alloc, std::size_t N, typename Runnable, typename ... Runnables>
		static void pack_runnable(std::array<job, N> & jobs, std::size_t & index, std::vector<job> & batchJobs, Runnable && runnable, Runnables&&... runnables) {
			pack_runnable<Alloc>(jobs, index, batchJobs, std::forward<Runnable>(runnable));
			pack_runnable<Alloc>(jobs, index, batchJobs, std::forward<Runnables>(runnables)...);
		}

		//Loads a Runnable into an envelope and pushes it to the given vector. Allocates.
		template<bool Alloc, typename Runnable, std::size_t N, std::enable_if_t<Alloc, int> = 0>
		static void pack_runnable(std::array<job, N> & jobs, std::size_t & index, std::vector<job> & batchJobs, Runnable&& runnable) {
			jobs[index++] = std::move(job{ std::forward<Runnable>(runnable) });
		}

		//Loads a Runnable into an envelope and pushes it to the given vector
		template<bool Alloc, typename Runnable, std::size_t N, std::enable_if_t<!Alloc, int> = 0>
		static void pack_runnable(std::array<job, N> & jobs, std::size_t & index, std::vector<job> & batchJobs, Runnable&& runnable) {
			jobs[index++] = { &runnable };
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector Allocates.
		template<bool Alloc, typename Callable, typename ... Params, std::size_t N, std::enable_if_t<Alloc, int> = 0>
		static void pack_runnable(std::array<job, N> & jobs, std::size_t & index, std::vector<job> & batchJobs, batch_function<Callable, Params...> && bf) {
			std::vector<job> splitEnvs = split_batch_function(std::forward<decltype(bf)>(bf));
			batchJobs.insert(batchJobs.end(), std::make_move_iterator(splitEnvs.begin()), std::make_move_iterator(splitEnvs.end()));
		}

		//Special overload for batch jobs - splits into envelopes and inserts them into the given vector
		template<bool Alloc, typename Callable, typename ... Params, std::size_t N, std::enable_if_t<!Alloc, int> = 0>
		static void pack_runnable(std::array<job, N> & envs, std::size_t & index, std::vector<job> & batchJobs, batch_function<Callable, Params...> && bf) {
			std::vector<job> splitEnvs = split_batch_function_no_alloc(bf);
			batchJobs.insert(batchJobs.end(), splitEnvs.begin(), splitEnvs.end());
		}

	}

#pragma endregion

#pragma region split_batch_function

	namespace impl{

		//Converts a BatchJob into a vector of Envelopes
		template<typename Callable, typename ... Params>
		static std::vector<job> split_batch_function(batch_function<Callable, Params...> && bf) {
			std::vector<job> jobs;
			jobs.reserve(bf.get_sections());
			typedef batch_function<Callable, Params...> ptrType;
			std::shared_ptr<ptrType> basePtr = std::make_shared<ptrType>(bf);
			for (unsigned int section = 0; section < bf.get_sections(); section++) {
				jobs.emplace_back(basePtr);
			}
			return jobs;
		}

		//Converts a BatchJob into a vector of Envelopes without copying the job to the heap
		template<typename Callable, typename ... Params>
		static std::vector<job> split_batch_function_no_alloc(batch_function<Callable, Params...> & bf) {
			std::vector<job> jobs;
			jobs.reserve(bf.get_sections());
			for (unsigned int section = 0; section < j.get_sections(); section++) {
				jobs.emplace_back(&bf);
			}
			return jobs;
		}

	}

#pragma endregion

	// Asynchronously invokes a set of Runnable objects.
	// If ToMain is true, the Runnables will be invoked on the main thread.
	template<bool ToMain = false, typename ... Runnables>
	void push(Runnables&&... runnables) {
		impl::push<ToMain, false>(std::forward<Runnables>(runnables)...);
	}

	// Asynchronously invokes a set of Runnable objects. If the current job was invoked synchronously, it will not return until these Runnables return.
	// If ToMain is true, the Runnables will be invoked on the main thread.
	template<bool ToMain = false, typename ... Runnables>
	void push_dependent(Runnables&&... runnables) {
		impl::push<ToMain, true>(std::forward<Runnables>(runnables)...);
	}

	// Synchronously invokes a set of Runnable objects.
	// If ToMain is true, the Runnables will be invoked on the main thread.
	// If FromMain is true, this function will return on the main thread.
	template<bool ToMain = false, bool FromMain = false, typename ... Runnables>
	void call(Runnables&&... runnables) {
		using namespace impl;
		std::array<job, sizeof...(Runnables)-batch_count<Runnables...>::value> jobs;
		std::vector<job> batchJobs;
		pack_runnable<false>(jobs, batchJobs, std::forward<Runnables>(runnables)...);
		impl::call<ToMain, FromMain>(std::move(jobs), std::move(batchJobs));
	}

	// Moves the current call stack to the main thread, then returns.
	inline void switch_to_main() {
		impl::call<false, true>();
	}

	// Invokes a Callable object once for each value between start (inclusive) and end (exclusive), passing the value to each invocation.
	template<typename Callable, typename ... Params>
	void parallel_for(unsigned start, unsigned end, Callable callable, Params... args) {
		call(bind_batch([&](unsigned start, unsigned end, Params... args) {
			for (BatchIndex c = start; c < end; c++)
				callable(c, args...);
		}, start, end, args...));
	}

	// Starts the job system with as many threads as the system can run concurrently and enters the given Callable with the given parameters. Returns when kill_all_workers is called.
	template <typename Callable, typename ... Params>
	void start_async(Callable callable, Params ... args) {
		start_async(std::thread::hardware_concurrency(), callable, args...);
	}

	// Starts the job system with the given number of threads and enters the given Callable with the given parameters. Returns when kill_all_workers is called.
	template <typename Callable, typename ... Params>
	void start_async(unsigned threadCount, Callable callable, Params ... args) {
		using namespace impl;

		//create threads
		std::vector<worker_thread> threads;

		threads.resize(threadCount - 1);

		ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);

		push<true>(bind(callable, args...));

		worker_thread::job_loop();

		for (worker_thread & wt : threads)
			wt.Join();
	}

	//Starts the job system with as many threads as the system can run concurrently and enters the given Callable with the given parameters. Returns when the Callable returns.
	template <typename Callable, typename ... Params>
	void start_sync(Callable callable, Params ... args) {
		start_sync(std::thread::hardware_concurrency(), callable, args...);
	}

	//Starts the job system with the given number of threads and enters the given Callable with the given parameters. Returns when the Callable returns.
	template <typename Callable, typename ... Params>
	void start_sync(unsigned threadCount, Callable callable, Params ... args) {
		using namespace impl;

		//create threads
		std::vector<worker_thread> threads;

		threads.resize(threadCount - 1);

		ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);

		nova::call<true, true>(bind(callable, args...));

		for (worker_thread & wt : threads)
			push(bind(worker_thread::kill_worker));
		for (worker_thread & wt : threads)
			wt.Join();
	}

	// Stops the job system, triggering a return from the start function. No invocations attempted after this one will occur.
	inline void kill_all_workers() {
		using namespace impl;
		for (unsigned c = 0; c < worker_thread::get_thread_count(); c++)
			nova::push(bind(worker_thread::kill_worker));
	}
}
