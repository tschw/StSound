#include "SoundServerOpenAL.h"

#include <pthread.h>
#include <time.h>

#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

struct CSoundServer::body
{
	static ALuint const n_buffers = 3;

	ALCcontext*		ptr_context;
	ALuint			arr_buffers[n_buffers];
	ALuint			val_source;
	ALuint			val_index;

	ALshort*		ptr_stream_buffer;
	size_t			val_stream_buffer_size;

	USER_CALLBACK	ptr_stream_func;
	unsigned		val_ms_latency;

	pthread_t		obj_thread;
	volatile bool	flg_stopped;

	pthread_mutex_t	mtx_sync;
	pthread_cond_t	cnd_sync;

	body(USER_CALLBACK stream_func, unsigned ms_latency)
	  : ptr_context(0l)
	  , flg_stopped(true)
	  , ptr_stream_buffer(0l)
	  , val_stream_buffer_size(44100*ms_latency/1000)
	  , ptr_stream_func(stream_func)
	  , val_ms_latency(ms_latency)
	{
		ptr_stream_buffer = new ALshort[val_stream_buffer_size];
		val_stream_buffer_size <<= 1; // in bytes, from now on
		pthread_mutex_init(&mtx_sync, 0l);
		pthread_cond_init(&cnd_sync, 0l);

		// create thread and sleep until after init
		pthread_create(&obj_thread, 0l, &thread, this);
		pthread_mutex_lock(&mtx_sync);
		pthread_cond_wait(&cnd_sync, &mtx_sync);
		pthread_mutex_unlock(&mtx_sync);
	}

	~body()
	{
		// stop thread - try to wake up
		pthread_mutex_lock(&mtx_sync);
		if (! flg_stopped)
		{
		  flg_stopped = true;
		  pthread_mutex_unlock(&mtx_sync);
		  //- pthread_cond_signal(&cnd_sync);
		  pthread_join(obj_thread, 0l);
		}
		else pthread_mutex_unlock(&mtx_sync);

		delete[] ptr_stream_buffer;
	}

	static void* thread(void* context)
	{
		static_cast<body*>(context)->main();
		return 0l;
	}

	bool is_running()
	{
		pthread_mutex_lock(&mtx_sync);
		bool result = !flg_stopped;
		pthread_mutex_unlock(&mtx_sync);
		return result;
	}

	void main()
	{
		pthread_mutex_lock(&mtx_sync);
		if (init_al())
		{
			start_streaming();
			flg_stopped = false;
			// notify other thread that init is over
			pthread_mutex_unlock(&mtx_sync);
			pthread_cond_signal(&cnd_sync);
			pthread_mutex_lock(&mtx_sync);
			while (!flg_stopped)
			{
				pthread_mutex_unlock(&mtx_sync);
				keep_streaming();

				timespec t = { 0, val_ms_latency*1000000/n_buffers };
				// pthread_cond_timedwait(&cnd_sync, &mtx_sync, &t);
				// would be nicer but it needs an absolute timeval and it takes
				// a lot of code to portably build it...
				nanosleep(& t, 0l);
				pthread_mutex_lock(&mtx_sync);
			}
		}
		pthread_mutex_unlock(&mtx_sync);
		// notify other thread that init is over (in case of an error)
		pthread_cond_signal(&cnd_sync);
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

		alGenSources(1, & val_source);
		val_index = 0;
		if(alGetError() != AL_NO_ERROR) return false;

		return true;
	}

	void close_al()
	{
		if (!ptr_context) return;
		alDeleteSources(1, & val_source);
		alDeleteBuffers(sizeof(arr_buffers)/sizeof(ALuint), arr_buffers);
		ALCdevice * device = alcGetContextsDevice(ptr_context);
		alcDestroyContext(ptr_context);
		alcCloseDevice(device);
	}

	void fill_buffers(ALuint first, ALuint n)
	{
		for (ALuint j = first, je = first+n; j < je; ++j)
		{
			(*ptr_stream_func)(ptr_stream_buffer, val_stream_buffer_size);

			alBufferData(arr_buffers[j], AL_FORMAT_MONO16, ptr_stream_buffer,
				val_stream_buffer_size, 44100);
		}
	}

	void start_streaming()
	{
		fill_buffers(0,n_buffers);
		alSourceQueueBuffers(val_source, n_buffers, &arr_buffers[0]);
		alSourcef(val_source, AL_GAIN, 1.0f);
		alListenerf(AL_GAIN, 1.0f);
		alSourcePlay(val_source);
	}

	void keep_streaming()
	{
		ALint p;
		alGetSourcei(val_source, AL_BUFFERS_PROCESSED, &p);

		ALint l1 = p, l2 = 0, next_index = val_index+p;
		if (next_index >= n_buffers)
		{
			l2 = next_index -= n_buffers;
			l1 = n_buffers - val_index;
		}
		alSourceUnqueueBuffers(val_source, l1, &arr_buffers[val_index]);
		alSourceUnqueueBuffers(val_source, l2, &arr_buffers[0]);
		fill_buffers(val_index, l1);
		fill_buffers(0, l2);
		alSourceQueueBuffers(val_source, l1, &arr_buffers[val_index]);
		alSourceQueueBuffers(val_source, l2, &arr_buffers[0]);

		val_index = next_index;
	}

};

CSoundServer::CSoundServer()
  : m_pBody(0l)
{
}

bool CSoundServer::open(USER_CALLBACK pUserCallback,long totalBufferedSoundLen)
{
	if (! m_pBody)
		m_pBody = new body(pUserCallback, totalBufferedSoundLen);
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


