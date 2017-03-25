/*
**  Renderer multithreading framework
**  Copyright (c) 2016 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#pragma once

#include "r_draw.h"
#include "polyrenderer/drawers/screen_triangle.h"
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

// Use multiple threads when drawing
EXTERN_CVAR(Bool, r_multithreaded)

// Worker data for each thread executing drawer commands
class DrawerThread
{
public:
	DrawerThread()
	{
		FullSpansBuffer.resize(MAXWIDTH / 8 * (MAXHEIGHT / 8));
		PartialBlocksBuffer.resize(MAXWIDTH / 8 * (MAXHEIGHT / 8));
	}

	std::thread thread;

	// Thread line index of this thread
	int core = 0;

	// Number of active threads
	int num_cores = 1;

	// Range of rows processed this pass
	int pass_start_y = 0;
	int pass_end_y = MAXHEIGHT;

	// Working buffer used by the tilted (sloped) span drawer
	const uint8_t *tiltlighting[MAXWIDTH];

	// Working buffer used by the triangler drawer
	std::vector<TriFullSpan> FullSpansBuffer;
	std::vector<TriPartialBlock> PartialBlocksBuffer;

	// Checks if a line is rendered by this thread
	bool line_skipped_by_thread(int line)
	{
		return line < pass_start_y || line >= pass_end_y || line % num_cores != core;
	}

	// The number of lines to skip to reach the first line to be rendered by this thread
	int skipped_by_thread(int first_line)
	{
		int pass_skip = MAX(pass_start_y - first_line, 0);
		int core_skip = (num_cores - (first_line + pass_skip - core) % num_cores) % num_cores;
		return pass_skip + core_skip;
	}

	// The number of lines to be rendered by this thread
	int count_for_thread(int first_line, int count)
	{
		int lines_until_pass_end = MAX(pass_end_y - first_line, 0);
		count = MIN(count, lines_until_pass_end);
		int c = (count - skipped_by_thread(first_line) + num_cores - 1) / num_cores;
		return MAX(c, 0);
	}

	// Calculate the dest address for the first line to be rendered by this thread
	template<typename T>
	T *dest_for_thread(int first_line, int pitch, T *dest)
	{
		return dest + skipped_by_thread(first_line) * pitch;
	}

	// The first line in the dc_temp buffer used this thread
	int temp_line_for_thread(int first_line)
	{
		return (first_line + skipped_by_thread(first_line)) / num_cores;
	}
};

// Task to be executed by each worker thread
class DrawerCommand
{
public:
	virtual ~DrawerCommand() { }

	virtual void Execute(DrawerThread *thread) = 0;
	virtual FString DebugInfo() = 0;
};

void VectoredTryCatch(void *data, void(*tryBlock)(void *data), void(*catchBlock)(void *data, const char *reason, bool fatal));

class DrawerCommandQueue;
typedef std::shared_ptr<DrawerCommandQueue> DrawerCommandQueuePtr;

class DrawerThreads
{
public:
	// Runs the collected commands on worker threads
	static void Execute(const std::vector<DrawerCommandQueuePtr> &queues);
	
private:
	DrawerThreads();
	~DrawerThreads();
	
	void StartThreads();
	void StopThreads();

	static DrawerThreads *Instance();
	static void ReportDrawerError(DrawerCommand *command, bool worker_thread, const char *reason, bool fatal);
	
	std::vector<DrawerThread> threads;

	std::mutex start_mutex;
	std::condition_variable start_condition;
	std::vector<DrawerCommandQueuePtr> active_commands;
	bool shutdown_flag = false;
	int run_id = 0;

	std::mutex end_mutex;
	std::condition_variable end_condition;
	size_t finished_threads = 0;
	FString thread_error;
	bool thread_error_fatal = false;

	int threaded_render = 0;
	DrawerThread single_core_thread;
	int num_passes = 1;
	int rows_in_pass = MAXHEIGHT;
	
	friend class DrawerCommandQueue;
};

namespace swrenderer { class RenderThread; }

class DrawerCommandQueue
{
public:
	DrawerCommandQueue(swrenderer::RenderThread *renderthread);
	
	void Clear() { commands.clear(); }
	
	// Queue command to be executed by drawer worker threads
	template<typename T, typename... Types>
	void Push(Types &&... args)
	{
		DrawerThreads *threads = DrawerThreads::Instance();
		if (ThreadedRender && r_multithreaded)
		{
			void *ptr = AllocMemory(sizeof(T));
			T *command = new (ptr)T(std::forward<Types>(args)...);
			commands.push_back(command);
		}
		else
		{
			T command(std::forward<Types>(args)...);
			command.Execute(&threads->single_core_thread);
		}
	}
	
	bool ThreadedRender = true;
	
private:
	// Allocate memory valid for the duration of a command execution
	void *AllocMemory(size_t size);
	
	std::vector<DrawerCommand *> commands;
	swrenderer::RenderThread *renderthread;
	
	friend class DrawerThreads;
};
