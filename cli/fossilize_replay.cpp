/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"

#include <cinttypes>
#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <string.h>
#include <chrono>	// VALVE
#include <queue>	// VALVE
#include <thread>	// VALVE
#include <mutex>	// VALVE
#include <condition_variable> // VALVE
#include <atomic>
#include <fstream>
#include <atomic>
#include <algorithm>

#ifdef __linux__
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <errno.h>

using namespace Fossilize;
using namespace std;

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
		string on_disk_pipeline_cache_path;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;

		// Carve out a range of which pipelines to replay.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index = 0;
		unsigned end_graphics_index = ~0u;
		unsigned start_compute_index = 0;
		unsigned end_compute_index = ~0u;
	};

	ThreadedReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_)
		: opts(opts_),
		  num_worker_threads(opts.num_threads), loop_count(opts.loop_count),
		  device_opts(device_opts_)
	{
		// Cannot use initializers for atomics.
		graphics_pipeline_ns.store(0);
		compute_pipeline_ns.store(0);
		shader_module_ns.store(0);
		graphics_pipeline_count.store(0);
		compute_pipeline_count.store(0);
		shader_module_count.store(0);

		thread_current_graphics_index = opts.start_graphics_index;
		thread_current_compute_index = opts.start_compute_index;

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		for (unsigned i = 0; i < num_worker_threads; i++)
			thread_pool.push_back(std::thread(&ThreadedReplayer::worker_thread, this));
	}

	void sync_worker_threads()
	{
		unique_lock<mutex> lock(pipeline_work_queue_mutex);
		work_done_condition.wait(lock, [&]() -> bool {
			return queued_count == completed_count;
		});
	}

	void worker_thread()
	{
		uint64_t graphics_ns = 0;
		unsigned graphics_count = 0;

		uint64_t compute_ns = 0;
		unsigned compute_count = 0;

		uint64_t shader_ns = 0;
		unsigned shader_count = 0;

		for (;;)
		{
			PipelineWorkItem work_item;
			{
				unique_lock<mutex> lock(pipeline_work_queue_mutex);
				work_available_condition.wait(lock, [&]() -> bool {
					return shutting_down || !pipeline_work_queue.empty();
				});

				if (shutting_down)
					break;

				work_item = pipeline_work_queue.front();
				pipeline_work_queue.pop();
			}

			switch (work_item.tag)
			{
			case RESOURCE_SHADER_MODULE:
			{
				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.shader_module != VK_NULL_HANDLE)
						vkDestroyShaderModule(device->get_device(), *work_item.hash_map_entry.shader_module, nullptr);
					*work_item.hash_map_entry.shader_module = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();
					if (vkCreateShaderModule(device->get_device(), work_item.create_info.shader_module_create_info,
					                         nullptr, work_item.output.shader_module) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						shader_module_ns += duration_ns;
						shader_module_count++;
						*work_item.hash_map_entry.shader_module = *work_item.output.shader_module;

						if (robustness)
							shader_module_to_hash[*work_item.output.shader_module] = work_item.hash;
					}
					else
					{
						LOGE("Failed to create shader module for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			case RESOURCE_GRAPHICS_PIPELINE:
			{
				if (robustness)
				{
					num_failed_module_hashes = work_item.create_info.graphics_create_info->stageCount;
					for (unsigned i = 0; i < work_item.create_info.graphics_create_info->stageCount; i++)
					{
						VkShaderModule module = work_item.create_info.graphics_create_info->pStages[i].module;
						failed_module_hashes[i] = shader_module_to_hash[module];
					}
				}

				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();

#if 1
					// Robustness checking.
					if ((thread_current_graphics_index & 7) == 7)
						pthread_kill(pthread_self(), SIGSEGV);
#endif

					if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, work_item.create_info.graphics_create_info,
					                              nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						graphics_pipeline_ns += duration_ns;
						graphics_pipeline_count++;
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						LOGE("Failed to create graphics pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}

				thread_current_graphics_index++;
				break;
			}

			case RESOURCE_COMPUTE_PIPELINE:
			{
				if (robustness)
				{
					num_failed_module_hashes = 1;
					VkShaderModule module = work_item.create_info.compute_create_info->stage.module;
					failed_module_hashes[0] = shader_module_to_hash[module];
				}

				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();

#if 1
					// Robustness checking.
					if ((thread_current_compute_index & 7) == 7)
						pthread_kill(pthread_self(), SIGSEGV);
#endif

					if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1,
					                             work_item.create_info.compute_create_info,
					                             nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						compute_pipeline_ns += duration_ns;
						compute_pipeline_count++;
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						LOGE("Failed to create compute pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}

				thread_current_compute_index++;
				break;
			}

			default:
				break;
			}

			{
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				completed_count++;
				if (completed_count == queued_count) // Makes sense to signal main thread now.
					work_done_condition.notify_one();
			}
		}

		graphics_pipeline_count.fetch_add(graphics_count, std::memory_order_relaxed);
		graphics_pipeline_ns.fetch_add(graphics_ns, std::memory_order_relaxed);
		compute_pipeline_count.fetch_add(compute_count, std::memory_order_relaxed);
		compute_pipeline_ns.fetch_add(compute_ns, std::memory_order_relaxed);
		shader_module_count.fetch_add(shader_count, std::memory_order_relaxed);
		shader_module_ns.fetch_add(shader_ns, std::memory_order_relaxed);
	}

	~ThreadedReplayer()
	{
		// Signal that it's time for threads to die.
		{
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			shutting_down = true;
			work_available_condition.notify_all();
		}

		for (auto &thread : thread_pool)
			if (thread.joinable())
				thread.join();

		if (pipeline_cache)
		{
			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				size_t pipeline_cache_size = 0;
				if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> pipeline_buffer(pipeline_cache_size);
					if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, pipeline_buffer.data()) == VK_SUCCESS)
					{
						FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "wb");
						if (!file || fwrite(pipeline_buffer.data(), 1, pipeline_buffer.size(), file) != pipeline_buffer.size())
							LOGE("Failed to write pipeline cache data to disk.\n");
						if (file)
							fclose(file);
					}
				}
			}
			vkDestroyPipelineCache(device->get_device(), pipeline_cache, nullptr);
		}

		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device->get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device->get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device->get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device->get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device->get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
	}

	bool validate_pipeline_cache_header(const vector<uint8_t> &blob)
	{
		if (blob.size() < 16 + VK_UUID_SIZE)
		{
			LOGI("Pipeline cache header is too small.\n");
			return false;
		}

		const auto read_le = [&](unsigned offset) -> uint32_t {
			return uint32_t(blob[offset + 0]) |
				(uint32_t(blob[offset + 1]) << 8) |
				(uint32_t(blob[offset + 2]) << 16) |
				(uint32_t(blob[offset + 3]) << 24);
		};

		auto length = read_le(0);
		if (length != 16 + VK_UUID_SIZE)
		{
			LOGI("Length of pipeline cache header is not as expected.\n");
			return false;
		}

		auto version = read_le(4);
		if (version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
		{
			LOGI("Version of pipeline cache header is not 1.\n");
			return false;
		}

		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device->get_gpu(), &props);
		if (props.vendorID != read_le(8))
		{
			LOGI("Mismatch of vendorID and cache vendorID.\n");
			return false;
		}

		if (props.deviceID != read_le(12))
		{
			LOGI("Mismatch of deviceID and cache deviceID.\n");
			return false;
		}

		if (memcmp(props.pipelineCacheUUID, blob.data() + 16, VK_UUID_SIZE) != 0)
		{
			LOGI("Mismatch between pipelineCacheUUID.\n");
			return false;
		}

		return true;
	}

	void set_application_info(const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		// TODO: Could use this to create multiple VkDevices for replay as necessary if app changes.

		if (!device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device.reset(new VulkanDevice);
			device_opts.application_info = app;
			device_opts.features = features;
			device_opts.need_disasm = false;
			auto start_device = chrono::steady_clock::now();
			if (!device->init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
			}

			if (opts.pipeline_cache)
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vector<uint8_t> on_disk_cache;

				// Try to load on-disk cache.
				if (!opts.on_disk_pipeline_cache_path.empty())
				{
					FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "rb");
					if (file)
					{
						fseek(file, 0, SEEK_END);
						size_t len = ftell(file);
						rewind(file);

						if (len != 0)
						{
							on_disk_cache.resize(len);
							if (fread(on_disk_cache.data(), 1, len, file) == len)
							{
								if (validate_pipeline_cache_header(on_disk_cache))
								{
									info.pInitialData = on_disk_cache.data();
									info.initialDataSize = on_disk_cache.size();
								}
								else
									LOGI("Failed to validate pipeline cache. Creating a blank one.\n");
							}
						}
					}
				}

				if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
				{
					LOGE("Failed to create pipeline cache, trying to create a blank one.\n");
					info.initialDataSize = 0;
					info.pInitialData = nullptr;
					if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create pipeline cache.\n");
						pipeline_cache = VK_NULL_HANDLE;
					}
				}
			}

			auto end_device = chrono::steady_clock::now();
			long time_ms = chrono::duration_cast<chrono::milliseconds>(end_device - start_device).count();
			LOGI("Creating Vulkan device took: %ld ms\n", time_ms);

			if (app)
			{
				LOGI("Replaying for application:\n");
				LOGI("  apiVersion: %u.%u.%u\n",
				     VK_VERSION_MAJOR(app->apiVersion),
				     VK_VERSION_MINOR(app->apiVersion),
				     VK_VERSION_PATCH(app->apiVersion));
				LOGI("  engineVersion: %u\n", app->engineVersion);
				LOGI("  applicationVersion: %u\n", app->applicationVersion);
				if (app->pEngineName)
					LOGI("  engineName: %s\n", app->pEngineName);
				if (app->pApplicationName)
					LOGI("  applicationName: %s\n", app->pApplicationName);
			}
		}
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		// Playback in-order.
		if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		// Playback in-order.
		if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		// Playback in-order.
		if (vkCreatePipelineLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		// Playback in-order.
		if (vkCreateRenderPass(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (masked_shader_modules.count(hash))
		{
			*module = VK_NULL_HANDLE;
			return true;
		}

		PipelineWorkItem work_item;
		work_item.hash = hash;
		work_item.tag = RESOURCE_SHADER_MODULE;
		work_item.output.shader_module = module;
		// Pointer to value in std::unordered_map remains fixed per spec (node-based).
		work_item.hash_map_entry.shader_module = &shader_modules[hash];
		work_item.create_info.shader_module_create_info = create_info;

		{
			// Pipeline parsing with pipeline creation.
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			pipeline_work_queue.push(work_item);
			work_available_condition.notify_one();
			queued_count++;
		}

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if (create_info->stage.module != VK_NULL_HANDLE &&
		    compute_pipeline_index >= opts.start_compute_index &&
		    compute_pipeline_index < opts.end_compute_index)
		{
			PipelineWorkItem work_item;
			work_item.hash = hash;
			work_item.tag = RESOURCE_COMPUTE_PIPELINE;
			work_item.output.pipeline = pipeline;
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &compute_pipelines[hash];
			work_item.create_info.compute_create_info = create_info;

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		compute_pipeline_index++;
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		bool valid_handles = true;
		for (uint32_t i = 0; i < create_info->stageCount; i++)
			if (create_info->pStages[i].module == VK_NULL_HANDLE)
				valid_handles = false;

		if (valid_handles &&
		    graphics_pipeline_index >= opts.start_graphics_index &&
		    graphics_pipeline_index < opts.end_graphics_index)
		{
			PipelineWorkItem work_item;
			work_item.hash = hash;
			work_item.tag = RESOURCE_GRAPHICS_PIPELINE;
			work_item.output.pipeline = pipeline;
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &graphics_pipelines[hash];
			work_item.create_info.graphics_create_info = create_info;

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		graphics_pipeline_index++;
		return true;
	}

	void sync_threads() override
	{
		sync_worker_threads();
	}

	// Support ignoring shader module which are known to cause crashes.
	void mask_shader_module(Hash hash)
	{
		masked_shader_modules.insert(hash);
	}

	const vector<thread> &get_threads() const
	{
		return thread_pool;
	}

	void emergency_teardown()
	{
		device.reset();
	}

	Options opts;
	unsigned graphics_pipeline_index = 0;
	unsigned compute_pipeline_index = 0;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	std::unordered_set<Hash> masked_shader_modules;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer
	struct PipelineWorkItem
	{
		Hash hash = 0;
		ResourceTag tag = RESOURCE_COUNT;

		union
		{
			const VkGraphicsPipelineCreateInfo *graphics_create_info;
			const VkComputePipelineCreateInfo *compute_create_info;
			const VkShaderModuleCreateInfo *shader_module_create_info;
		} create_info = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} output = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} hash_map_entry = {};
	};

	unsigned num_worker_threads = 0;
	unsigned loop_count = 0;
	unsigned queued_count = 0;
	unsigned completed_count = 0;
	std::vector<std::thread> thread_pool;
	std::mutex pipeline_work_queue_mutex;
	std::queue<PipelineWorkItem> pipeline_work_queue;
	std::condition_variable work_available_condition;
	std::condition_variable work_done_condition;

	// Feed statistics from the worker threads.
	std::atomic<std::uint64_t> graphics_pipeline_ns;
	std::atomic<std::uint64_t> compute_pipeline_ns;
	std::atomic<std::uint64_t> shader_module_ns;
	std::atomic<std::uint32_t> graphics_pipeline_count;
	std::atomic<std::uint32_t> compute_pipeline_count;
	std::atomic<std::uint32_t> shader_module_count;

	bool shutting_down = false;

	unique_ptr<VulkanDevice> device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;

	// Crash recovery.
	Hash failed_module_hashes[6];
	unsigned num_failed_module_hashes = 0;
	unsigned thread_current_graphics_index = 0;
	unsigned thread_current_compute_index = 0;
	bool robustness = false;
};

static void print_help()
{
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--filter-compute <index>]\n"
	     "\t[--filter-graphics <index>]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t[--graphics-pipeline-range <start> <end>]\n"
	     "\t[--compute-pipeline-range <start> <end>]\n"
	     "\t[--slave-process]\n"
	     "\t[--master-process]\n"
	     "\t<Database>\n");
}

#ifdef __linux__
static bool write_all(int fd, const char *str)
{
	// write is async-signal safe, but not stdio.
	size_t len = strlen(str);
	while (len)
	{
		ssize_t wrote = write(fd, str, len);
		if (wrote <= 0)
			return false;

		str += wrote;
		len -= wrote;
	}

	return true;
}
#endif

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const string &db_path);

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts_,
                              const string &db_path)
{
	auto replayer_opts = replayer_opts_;
	unsigned processes = replayer_opts.num_threads;
	replayer_opts.num_threads = 1;


	size_t num_graphics_pipelines;
	size_t num_compute_pipelines;
	{
		auto db = unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
		if (!db->prepare())
			return EXIT_FAILURE;
		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &num_graphics_pipelines, nullptr))
			return EXIT_FAILURE;
		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &num_compute_pipelines, nullptr))
			return EXIT_FAILURE;
	}

	unordered_set<Hash> faulty_spirv_modules;

#ifdef __linux__
	struct ProcessProgress
	{
		unsigned end_graphics_index = ~0u;
		unsigned end_compute_index = ~0u;
		pid_t pid = -1;
		int crash_fd = -1;
	};

	const auto send_faulty_modules_and_close = [&](int fd)
	{
		for (auto &m : faulty_spirv_modules)
		{
			char buffer[18];
			sprintf(buffer, "%llx\n", static_cast<unsigned long long>(m));
			write_all(fd, buffer);
		}

		close(fd);
	};

	// We will wait for child processes explicitly.
	signal(SIGCHLD, SIG_IGN);

	// fork() and pipe() strategy.
	vector<ProcessProgress> child_processes;
	unsigned active_processes = 0;
	for (unsigned i = 0; i < processes; i++)
	{
		replayer_opts.start_graphics_index = (i * unsigned(num_graphics_pipelines)) / processes;
		replayer_opts.end_graphics_index = ((i + 1) * unsigned(num_graphics_pipelines)) / processes;
		replayer_opts.start_compute_index = (i * unsigned(num_compute_pipelines)) / processes;
		replayer_opts.end_compute_index = ((i + 1) * unsigned(num_compute_pipelines)) / processes;

		int crash_fds[2];
		int input_fds[2];
		if (pipe(crash_fds) < 0)
			return EXIT_FAILURE;
		if (pipe(input_fds) < 0)
			return EXIT_FAILURE;

		pid_t pid = fork(); // Fork off a child.
		if (pid)
		{
			// We're the parent, keep track of the process.
			ProcessProgress progress;
			progress.crash_fd = crash_fds[0];
			send_faulty_modules_and_close(input_fds[1]);
			close(crash_fds[1]);
			close(input_fds[0]);
			progress.end_graphics_index = replayer_opts.end_graphics_index;
			progress.end_compute_index = replayer_opts.end_compute_index;
			child_processes.push_back(progress);
			active_processes++;
		}
		else
		{
			// We're the child process.
			close(crash_fds[0]);
			close(input_fds[1]);
			if (dup2(crash_fds[1], STDOUT_FILENO) < 0)
				return EXIT_FAILURE;
			if (dup2(input_fds[0], STDIN_FILENO) < 0)
				return EXIT_FAILURE;
			return run_slave_process(opts, replayer_opts, db_path);
		}
	}

	while (active_processes)
	{
		int wstatus;
		pid_t pid = wait(&wstatus);
		if (pid < 0)
			return EXIT_FAILURE;

		auto itr = std::find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &progress) -> bool {
			return progress.pid == pid;
		});

		if (itr == end(child_processes))
		{
			// Can this happen? Just ignore ...
			continue;
		}

		active_processes--;
		handle_end_of_process(*itr);
	}
#endif

	return EXIT_FAILURE;
}

static int run_normal_process(ThreadedReplayer &replayer, const string &db_path)
{
	auto start_time = chrono::steady_clock::now();
	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
	auto end_create_archive = chrono::steady_clock::now();

	auto start_prepare = chrono::steady_clock::now();
	if (!resolver->prepare())
	{
		LOGE("Failed to prepare database.\n");
		return EXIT_FAILURE;
	}
	auto end_prepare = chrono::steady_clock::now();

	StateReplayer state_replayer;

	vector<Hash> resource_hashes;
	vector<uint8_t> state_json;

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SHADER_MODULE, // Kick off shader modules first since it can be done in a thread while we deal with trivial objects.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
		RESOURCE_GRAPHICS_PIPELINE, // Multi-threaded
		RESOURCE_COMPUTE_PIPELINE, // Multi-threaded
	};

	for (auto &tag : playback_order)
	{
		size_t resource_hash_count = 0;
		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, nullptr))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		resource_hashes.resize(resource_hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, resource_hashes.data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto &hash : resource_hashes)
		{
			size_t state_json_size = 0;
			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);

			if (!resolver->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			try
			{
				state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size());
			}
			catch (const exception &e)
			{
				LOGE("StateReplayer threw exception parsing (tag: %d, hash: 0x%llx): %s\n", tag, static_cast<unsigned long long>(hash), e.what());
			}
		}

		// Before continuing with pipelines, make sure the threaded shader modules have been created.
		if (tag == RESOURCE_RENDER_PASS)
			replayer.sync_worker_threads();
	}

	// VALVE: drain all outstanding pipeline compiles
	replayer.sync_worker_threads();

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	LOGI("Playing back %u shader modules took %.3f s (accumulated time)\n",
	     replayer.shader_module_count.load(),
	     replayer.shader_module_ns.load() * 1e-9);

	LOGI("Playing back %u graphics pipelines took %.3f s (accumulated time)\n",
	     replayer.graphics_pipeline_count.load(),
	     replayer.graphics_pipeline_ns.load() * 1e-9);

	LOGI("Playing back %u compute pipelines took %.3f s (accumulated time)\n",
	     replayer.compute_pipeline_count.load(),
	     replayer.compute_pipeline_ns.load() * 1e-9);

	LOGI("Replayed %lu objects in %ld ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}

#ifdef __linux__

static ThreadedReplayer *global_replayer = nullptr;
static int crash_fd;

static void crash_handler(int)
{
	// stderr is reserved for generic logging.
	// stdout/stdin is for IPC with master process.

	if (!write_all(crash_fd, "CRASH\n"))
		_exit(2);

	// This might hang indefinitely if we are exceptionally unlucky,
	// the parent will have a timeout after receiving the crash message.
	// If that fails, it can SIGKILL us.
	// We want to make sure any database writing threads in the driver gets a chance to complete its work
	// before we die.

	if (global_replayer)
	{
		char buffer[64];

		// Report to parent process which VkShaderModule's might have contributed to our untimely death.
		// This allows a new process to ignore these modules.
		for (unsigned i = 0; i < global_replayer->num_failed_module_hashes; i++)
		{
			sprintf(buffer, "MODULE %llx\n",
					static_cast<unsigned long long>(global_replayer->failed_module_hashes[i]));
			if (!write_all(crash_fd, buffer))
				_exit(2);
		}

		// Report where we stopped, so we can continue.
		sprintf(buffer, "GRAPHICS %d\n", global_replayer->thread_current_graphics_index);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		sprintf(buffer, "COMPUTE %d\n", global_replayer->thread_current_compute_index);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		global_replayer->emergency_teardown();
	}

	// Clean exit instead of reporting the segfault.
	// _exit is async-signal safe, but not exit().
	// Use exit code 2 to mark a segfaulted child.
	_exit(2);
}
#endif

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const string &db_path)
{
	ThreadedReplayer replayer(opts, replayer_opts);
	replayer.robustness = true;

	// In slave mode, we can receive a list of shader module hashes we should ignore.
	// This is to avoid trying to replay the same faulty shader modules again and again.
	char ignored_shader_module_hash[16 + 2];
	while (fgets(ignored_shader_module_hash, sizeof(ignored_shader_module_hash), stdin))
	{
		errno = 0;
		auto hash = strtoull(ignored_shader_module_hash, nullptr, 16);
		if (hash == 0)
			break;
		if (errno == 0)
		{
			LOGE("Ignoring module %llx\n", hash);
			replayer.mask_shader_module(Hash(hash));
		}
	}

#ifdef __linux__
	// Make sure that the driver cannot mess up the master process.
	// Just stdout.
	crash_fd = dup(STDOUT_FILENO);
	close(STDOUT_FILENO);

	global_replayer = &replayer;

	// Just in case the driver crashed due to stack overflow,
	// provide an alternate stack where we can clean up "safely".
	stack_t ss;
	ss.ss_sp = malloc(1024 * 1024);
	ss.ss_size = 1024 * 1024;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, nullptr) < 0)
		return EXIT_FAILURE;

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);
	act.sa_handler = crash_handler;
	act.sa_flags = SA_RESETHAND | SA_ONSTACK;

	if (sigaction(SIGSEGV, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGFPE, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGILL, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGBUS, &act, nullptr) < 0)
		return EXIT_FAILURE;

#if 0
	// Don't let main thread handle the signals.
	// Must be handled by the threads themselves.
	// I think SIGSEGV should always be handled on the offending thread,
	// but need this for testing at least.
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGSEGV);
	sigaddset(&set, SIGFPE);
	sigaddset(&set, SIGILL);
	sigaddset(&set, SIGBUS);
	if (pthread_sigmask(SIG_BLOCK, &set, nullptr) < 0)
		return EXIT_FAILURE;
#endif

	return run_normal_process(replayer, db_path);
#else
	ThreadedReplayer replayer(opts, replayer_opts);
	return run_normal_process(replayer, db_path);
#endif
}

int main(int argc, char *argv[])
{
	string db_path;
	VulkanDevice::Options opts;
	ThreadedReplayer::Options replayer_opts;
	bool master_process = false;
	bool slave_process = false;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { db_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--master-process", [&](CLIParser &) { master_process = true; });
	cbs.add("--slave-process", [&](CLIParser &) { slave_process = true; });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });

	cbs.add("--graphics-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_graphics_index = parser.next_uint();
		replayer_opts.end_graphics_index = parser.next_uint();
	});

	cbs.add("--compute-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_compute_index = parser.next_uint();
		replayer_opts.end_compute_index = parser.next_uint();
	});

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (db_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	// We cannot safely deal with multiple threads here, force one thread.
	if (slave_process)
	{
		if (replayer_opts.num_threads > 1)
			LOGE("Cannot use more than one thread per slave process. Forcing 1 thread.\n");
		replayer_opts.num_threads = 1;
	}

	if (replayer_opts.num_threads < 1)
		replayer_opts.num_threads = 1;

	if (!replayer_opts.on_disk_pipeline_cache_path.empty())
		replayer_opts.pipeline_cache = true;

	int ret;
	if (master_process)
		ret = run_master_process(opts, replayer_opts, db_path);
	else if (slave_process)
		ret = run_slave_process(opts, replayer_opts, db_path);
	else
	{
		ThreadedReplayer replayer(opts, replayer_opts);
		ret = run_normal_process(replayer, db_path);
	}
	return ret;
}
