#include "SoundServerOpenAL.h"

#include <stdint.h>
#include <functional>

#include <thread>
#include <chrono>
#include <atomic>

#include <mutex>
#include <condition_variable>


#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

using lock = std::unique_lock<std::mutex>;
using callback = std::function< void(int16_t*,size_t) >;

class CSoundServer::body
{
	static ALuint const n_buffers = 3;

	ALCcontext*						ptr_context;
	ALuint							arr_buffers[n_buffers];
	ALuint							hnd_source;
	ALuint							val_index;
	ALshort*						ptr_stream_buffer;
	size_t							val_stream_buffer_size;

	callback						fnc_stream;
	unsigned						val_ms_latency;

	std::thread*					ptr_thread;
	std::atomic<int> volatile		enm_thread_state;
	enum thread_state
			{ birth, life, death };

	std::mutex						mtx_sync;
	std::condition_variable			cnd_sync;
public:

	body(callback&& func, unsigned ms_latency)
	  : ptr_context(0l)
	  , ptr_stream_buffer(0l)
	  , val_stream_buffer_size(44100*ms_latency/1000)
	  , fnc_stream(func)
	  , val_ms_latency(ms_latency)
	  , ptr_thread(0l)
	  , enm_thread_state(birth)
	{
		ptr_stream_buffer = new ALshort[val_stream_buffer_size];
		ptr_thread = new std::thread([this] { this->thread_main(); });
		{	lock l(mtx_sync);
			cnd_sync.wait(l,
					[this] () { return this->enm_thread_state != birth; });
		}
	}

	~body()
	{
		enm_thread_state = death;
		{	lock l(mtx_sync);
			cnd_sync.notify_one();
		}

		ptr_thread->join();

		delete ptr_thread;
		delete[] ptr_stream_buffer;
	}

	bool is_running() const
	{
		return enm_thread_state == life;
	}

private:
	void thread_main()
	{
		bool ok = init_al();

		enm_thread_state = ok ? life : death;
		{	lock l(mtx_sync);
			cnd_sync.notify_one();
		}

		if (ok)
		{
			start_streaming();

			while (enm_thread_state == life)
			{
				{	lock l(mtx_sync);
					using ms = std::chrono::milliseconds;
					cnd_sync.wait_for(l, ms(val_ms_latency/n_buffers));
				}
				keep_streaming();
			}
		}
		close_al();
	}

	bool init_al()
	{
		ALCdevice * device = alcOpenDevice(0l);
		if (!device) return false;

		ptr_context = alcCreateContext(device, 0);
		if (!ptr_context)
		{
			alcCloseDevice(device);
			return false;
		}
		alcMakeContextCurrent(ptr_context);

		alGetError();

		alGenBuffers(sizeof(arr_buffers)/sizeof(ALuint), arr_buffers);
		if(alGetError() != AL_NO_ERROR) return false;

		alGenSources(1, & hnd_source);
		val_index = 0;
		if(alGetError() != AL_NO_ERROR) return false;

		return true;
	}

	void close_al()
	{
		if (!ptr_context) return;
		alDeleteSources(1, & hnd_source);
		alDeleteBuffers(sizeof(arr_buffers)/sizeof(ALuint), arr_buffers);
		ALCdevice * device = alcGetContextsDevice(ptr_context);
		alcDestroyContext(ptr_context);
		alcCloseDevice(device);
	}

	void fill_buffers(ALuint first, ALuint n)
	{
		for (ALuint j = first, je = first+n; j < je; ++j)
		{
			fnc_stream(ptr_stream_buffer,val_stream_buffer_size);

			alBufferData(arr_buffers[j], AL_FORMAT_MONO16,
					ptr_stream_buffer, val_stream_buffer_size*2, 44100);
		}
	}

	void start_streaming()
	{
		fill_buffers(0,n_buffers);
		alSourceQueueBuffers(hnd_source, n_buffers, &arr_buffers[0]);
		alSourcef(hnd_source, AL_GAIN, 1.0f);
		alListenerf(AL_GAIN, 1.0f);
		alSourcePlay(hnd_source);
	}

	void keep_streaming()
	{
		ALint p;
		alGetSourcei(hnd_source, AL_BUFFERS_PROCESSED, & p);

		ALint l1 = p, l2 = 0, next_index = val_index + p;
		if (next_index >= n_buffers)
		{
			l2 = next_index -= n_buffers;
			l1 = n_buffers - val_index;
		}
		alSourceUnqueueBuffers(hnd_source, l1, &arr_buffers[val_index]);
		alSourceUnqueueBuffers(hnd_source, l2, &arr_buffers[0]);
		fill_buffers(val_index, l1);
		fill_buffers(0, l2);
		alSourceQueueBuffers(hnd_source, l1, &arr_buffers[val_index]);
		alSourceQueueBuffers(hnd_source, l2, &arr_buffers[0]);

		val_index = next_index;
	}
};

CSoundServer::CSoundServer()
  : m_pBody(0l)
{
}

bool CSoundServer::open(USER_CALLBACK pUserCallback,long bufferedMilliseconds)
{
	if (! m_pBody)
		m_pBody = new body([pUserCallback](int16_t* d,size_t n) {
					(*pUserCallback)((int16_t*)d,n*2); }, bufferedMilliseconds);
	return IsRunning();
}

CSoundServer::~CSoundServer()
{
	close();
}

void CSoundServer::close(void)
{
	delete m_pBody;
	m_pBody = 0l;
}

bool CSoundServer::IsRunning()
{
	return !!m_pBody && m_pBody->is_running();
}
