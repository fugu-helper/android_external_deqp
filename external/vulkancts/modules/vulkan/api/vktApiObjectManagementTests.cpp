/*-------------------------------------------------------------------------
 * Vulkan Conformance Tests
 * ------------------------
 *
 * Copyright (c) 2015 Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and/or associated documentation files (the
 * "Materials"), to deal in the Materials without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Materials, and to
 * permit persons to whom the Materials are furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice(s) and this permission notice shall be
 * included in all copies or substantial portions of the Materials.
 *
 * The Materials are Confidential Information as defined by the
 * Khronos Membership Agreement until designated non-confidential by
 * Khronos, at which point this condition clause shall be removed.
 *
 * THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
 *
 *//*!
 * \file
 * \brief Object management tests
 *//*--------------------------------------------------------------------*/

#include "vktApiObjectManagementTests.hpp"
#include "vktTestCaseUtil.hpp"

#include "vkDefs.hpp"
#include "vkRef.hpp"
#include "vkRefUtil.hpp"
#include "vkQueryUtil.hpp"
#include "vkMemUtil.hpp"
#include "vkPrograms.hpp"
#include "vkTypeUtil.hpp"
#include "vkPlatform.hpp"
#include "vkStrUtil.hpp"

#include "tcuVector.hpp"
#include "tcuResultCollector.hpp"
#include "tcuCommandLine.hpp"
#include "tcuTestLog.hpp"

#include "deUniquePtr.hpp"
#include "deSharedPtr.hpp"
#include "deArrayUtil.hpp"
#include "deSpinBarrier.hpp"
#include "deThread.hpp"
#include "deInt32.h"

namespace vkt
{
namespace api
{

namespace
{

using namespace vk;

using de::UniquePtr;
using de::MovePtr;
using de::SharedPtr;

using tcu::IVec3;
using tcu::UVec3;
using tcu::ResultCollector;
using tcu::TestStatus;
using tcu::TestLog;

using std::string;
using std::vector;

class ThreadGroupThread;

/*--------------------------------------------------------------------*//*!
 * \brief Thread group
 *
 * Thread group manages collection of threads that are expected to be
 * launched simultaneously as a group.
 *
 * Shared barrier is provided for synchronizing execution. Terminating thread
 * early either by returning from ThreadGroupThread::runThread() or throwing
 * an exception is safe, and other threads will continue execution. The
 * thread that has been terminated is simply removed from the synchronization
 * group.
 *
 * TestException-based exceptions are collected and translated into a
 * tcu::TestStatus by using tcu::ResultCollector.
 *
 * Use cases for ThreadGroup include for example testing thread-safety of
 * certain API operations by poking API simultaneously from multiple
 * threads.
 *//*--------------------------------------------------------------------*/
class ThreadGroup
{
public:
							ThreadGroup			(void);
							~ThreadGroup		(void);

	void					add					(de::MovePtr<ThreadGroupThread> thread);
	TestStatus				run					(void);

private:
	typedef std::vector<de::SharedPtr<ThreadGroupThread> >	ThreadVector;

	ThreadVector			m_threads;
	de::SpinBarrier			m_barrier;
} DE_WARN_UNUSED_TYPE;

class ThreadGroupThread : private de::Thread
{
public:
							ThreadGroupThread	(void);
	virtual					~ThreadGroupThread	(void);

	void					start				(de::SpinBarrier* groupBarrier);

	ResultCollector&		getResultCollector	(void) { return m_resultCollector; }

	using de::Thread::join;

protected:
	virtual void			runThread			(void) = 0;

	void					barrier				(void);

private:
							ThreadGroupThread	(const ThreadGroupThread&);
	ThreadGroupThread&		operator=			(const ThreadGroupThread&);

	void					run					(void);

	ResultCollector			m_resultCollector;
	de::SpinBarrier*		m_barrier;
};

// ThreadGroup

ThreadGroup::ThreadGroup (void)
	: m_barrier(1)
{
}

ThreadGroup::~ThreadGroup (void)
{
}

void ThreadGroup::add (de::MovePtr<ThreadGroupThread> thread)
{
	m_threads.push_back(de::SharedPtr<ThreadGroupThread>(thread.release()));
}

tcu::TestStatus ThreadGroup::run (void)
{
	tcu::ResultCollector	resultCollector;

	m_barrier.reset((int)m_threads.size());

	for (ThreadVector::iterator threadIter = m_threads.begin(); threadIter != m_threads.end(); ++threadIter)
		(*threadIter)->start(&m_barrier);

	for (ThreadVector::iterator threadIter = m_threads.begin(); threadIter != m_threads.end(); ++threadIter)
	{
		tcu::ResultCollector&	threadResult	= (*threadIter)->getResultCollector();
		(*threadIter)->join();
		resultCollector.addResult(threadResult.getResult(), threadResult.getMessage());
	}

	return tcu::TestStatus(resultCollector.getResult(), resultCollector.getMessage());
}

// ThreadGroupThread

ThreadGroupThread::ThreadGroupThread (void)
	: m_barrier(DE_NULL)
{
}

ThreadGroupThread::~ThreadGroupThread (void)
{
}

void ThreadGroupThread::start (de::SpinBarrier* groupBarrier)
{
	m_barrier = groupBarrier;
	de::Thread::start();
}

void ThreadGroupThread::run (void)
{
	try
	{
		runThread();
	}
	catch (const tcu::TestException& e)
	{
		getResultCollector().addResult(e.getTestResult(), e.getMessage());
	}
	catch (const std::exception& e)
	{
		getResultCollector().addResult(QP_TEST_RESULT_FAIL, e.what());
	}
	catch (...)
	{
		getResultCollector().addResult(QP_TEST_RESULT_FAIL, "Exception");
	}

	m_barrier->removeThread(de::SpinBarrier::WAIT_MODE_AUTO);
}

inline void ThreadGroupThread::barrier (void)
{
	m_barrier->sync(de::SpinBarrier::WAIT_MODE_AUTO);
}

deUint32 getDefaultTestThreadCount (void)
{
	return de::clamp(deGetNumAvailableLogicalCores(), 2u, 8u);
}

// Utilities

struct Environment
{
	const PlatformInterface&	vkp;
	const DeviceInterface&		vkd;
	VkDevice					device;
	deUint32					queueFamilyIndex;
	const BinaryCollection&		programBinaries;
	deUint32					maxResourceConsumers;		// Maximum number of objects using same Object::Resources concurrently

	Environment (Context& context, deUint32 maxResourceConsumers_)
		: vkp					(context.getPlatformInterface())
		, vkd					(context.getDeviceInterface())
		, device				(context.getDevice())
		, queueFamilyIndex		(context.getUniversalQueueFamilyIndex())
		, programBinaries		(context.getBinaryCollection())
		, maxResourceConsumers	(maxResourceConsumers_)
	{
	}

	Environment (const PlatformInterface&	vkp_,
				 const DeviceInterface&		vkd_,
				 VkDevice					device_,
				 deUint32					queueFamilyIndex_,
				 const BinaryCollection&	programBinaries_,
				 deUint32					maxResourceConsumers_)
		: vkp					(vkp_)
		, vkd					(vkd_)
		, device				(device_)
		, queueFamilyIndex		(queueFamilyIndex_)
		, programBinaries		(programBinaries_)
		, maxResourceConsumers	(maxResourceConsumers_)
	{
	}
};

template<typename Case>
struct Dependency
{
	typename Case::Resources		resources;
	Unique<typename Case::Type>		object;

	Dependency (const Environment& env, const typename Case::Parameters& params)
		: resources	(env, params)
		, object	(Case::create(env, resources, params))
	{}
};

// Object definitions

enum
{
	DEFAULT_MAX_CONCURRENT_OBJECTS	= 16*1024
};

struct Instance
{
	typedef VkInstance Type;

	enum { MAX_CONCURRENT = 32 };

	struct Parameters
	{
		Parameters (void) {}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkInstance> create (const Environment& env, const Resources&, const Parameters&)
	{
		const VkApplicationInfo		appInfo			=
		{
			VK_STRUCTURE_TYPE_APPLICATION_INFO,
			DE_NULL,
			DE_NULL,							// pApplicationName
			0u,									// applicationVersion
			DE_NULL,							// pEngineName
			0u,									// engineVersion
			VK_API_VERSION
		};
		const VkInstanceCreateInfo	instanceInfo	=
		{
			VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			DE_NULL,
			(VkInstanceCreateFlags)0,
			&appInfo,
			0u,									// enabledLayerNameCount
			DE_NULL,							// ppEnabledLayerNames
			0u,									// enabledExtensionNameCount
			DE_NULL,							// ppEnabledExtensionNames
		};

		return createInstance(env.vkp, &instanceInfo);
	}
};

struct Device
{
	typedef VkDevice Type;

	enum { MAX_CONCURRENT = 32 };

	struct Parameters
	{
		deUint32		deviceIndex;
		VkQueueFlags	queueFlags;

		Parameters (deUint32 deviceIndex_, VkQueueFlags queueFlags_)
			: deviceIndex	(deviceIndex_)
			, queueFlags	(queueFlags_)
		{}
	};

	struct Resources
	{
		Dependency<Instance>	instance;
		InstanceDriver			vki;
		VkPhysicalDevice		physicalDevice;
		deUint32				queueFamilyIndex;

		Resources (const Environment& env, const Parameters& params)
			: instance			(env, Instance::Parameters())
			, vki				(env.vkp, *instance.object)
			, physicalDevice	(0)
			, queueFamilyIndex	(~0u)
		{
			{
				const vector<VkPhysicalDevice>	physicalDevices	= enumeratePhysicalDevices(vki, *instance.object);

				if (physicalDevices.size() <= (size_t)params.deviceIndex)
					TCU_THROW(NotSupportedError, "Device not found");

				physicalDevice = physicalDevices[params.deviceIndex];
			}

			{
				const vector<VkQueueFamilyProperties>	queueProps		= getPhysicalDeviceQueueFamilyProperties(vki, physicalDevice);
				bool									foundMatching	= false;

				for (size_t curQueueNdx = 0; curQueueNdx < queueProps.size(); curQueueNdx++)
				{
					if ((queueProps[curQueueNdx].queueFlags & params.queueFlags) == params.queueFlags)
					{
						queueFamilyIndex	= (deUint32)curQueueNdx;
						foundMatching		= true;
					}
				}

				if (!foundMatching)
					TCU_THROW(NotSupportedError, "Matching queue not found");
			}
		}
	};

	static Move<VkDevice> create (const Environment&, const Resources& res, const Parameters&)
	{
		const float	queuePriority	= 1.0;

		const VkDeviceQueueCreateInfo	queues[]	=
		{
			{
				VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				DE_NULL,
				(VkDeviceQueueCreateFlags)0,
				res.queueFamilyIndex,
				1u,									// queueCount
				&queuePriority,						// pQueuePriorities
			}
		};
		const VkDeviceCreateInfo	deviceInfo	=
		{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			DE_NULL,
			(VkDeviceCreateFlags)0,
			DE_LENGTH_OF_ARRAY(queues),
			queues,
			0u,										// enabledLayerNameCount
			DE_NULL,								// ppEnabledLayerNames
			0u,										// enabledExtensionNameCount
			DE_NULL,								// ppEnabledExtensionNames
			DE_NULL,								// pEnabledFeatures
		};

		return createDevice(res.vki, res.physicalDevice, &deviceInfo);
	}
};

struct DeviceMemory
{
	typedef VkDeviceMemory Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkDeviceSize	size;
		deUint32		memoryTypeIndex;

		Parameters (VkDeviceSize size_, deUint32 memoryTypeIndex_)
			: size				(size_)
			, memoryTypeIndex	(memoryTypeIndex_)
		{
			DE_ASSERT(memoryTypeIndex < VK_MAX_MEMORY_TYPES);
		}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkDeviceMemory> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkMemoryAllocateInfo	allocInfo	=
		{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			DE_NULL,
			params.size,
			params.memoryTypeIndex
		};

		return allocateMemory(env.vkd, env.device, &allocInfo);
	}
};

DeviceMemory::Parameters getDeviceMemoryParameters (const VkMemoryRequirements& memReqs)
{
	return DeviceMemory::Parameters(memReqs.size, deCtz32(memReqs.memoryTypeBits));
}

DeviceMemory::Parameters getDeviceMemoryParameters (const Environment& env, VkImage image)
{
	return getDeviceMemoryParameters(getImageMemoryRequirements(env.vkd, env.device, image));
}

DeviceMemory::Parameters getDeviceMemoryParameters (const Environment& env, VkBuffer image)
{
	return getDeviceMemoryParameters(getBufferMemoryRequirements(env.vkd, env.device, image));
}

struct Buffer
{
	typedef VkBuffer Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkDeviceSize		size;
		VkBufferUsageFlags	usage;

		Parameters (VkDeviceSize		size_,
					VkBufferUsageFlags	usage_)
			: size	(size_)
			, usage	(usage_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkBuffer> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkBufferCreateInfo	bufferInfo	=
		{
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			DE_NULL,
			(VkBufferCreateFlags)0,
			params.size,
			params.usage,
			VK_SHARING_MODE_EXCLUSIVE,
			1u,
			&env.queueFamilyIndex
		};

		return createBuffer(env.vkd, env.device, &bufferInfo);
	}
};

struct BufferView
{
	typedef VkBufferView Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		Buffer::Parameters	buffer;
		VkFormat			format;
		VkDeviceSize		offset;
		VkDeviceSize		range;

		Parameters (const Buffer::Parameters&	buffer_,
					VkFormat					format_,
					VkDeviceSize				offset_,
					VkDeviceSize				range_)
			: buffer	(buffer_)
			, format	(format_)
			, offset	(offset_)
			, range		(range_)
		{}
	};

	struct Resources
	{
		Dependency<Buffer>			buffer;
		Dependency<DeviceMemory>	memory;

		Resources (const Environment& env, const Parameters& params)
			: buffer(env, params.buffer)
			, memory(env, getDeviceMemoryParameters(env, *buffer.object))
		{
			VK_CHECK(env.vkd.bindBufferMemory(env.device, *buffer.object, *memory.object, 0));
		}
	};

	static Move<VkBufferView> create (const Environment& env, const Resources& res, const Parameters& params)
	{
		const VkBufferViewCreateInfo	bufferViewInfo	=
		{
			VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
			DE_NULL,
			(VkBufferViewCreateFlags)0,
			*res.buffer.object,
			params.format,
			params.offset,
			params.range
		};

		return createBufferView(env.vkd, env.device, &bufferViewInfo);
	}
};

struct Image
{
	typedef VkImage Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkImageType				imageType;
		VkFormat				format;
		VkExtent3D				extent;
		deUint32				mipLevels;
		deUint32				arraySize;
		VkSampleCountFlagBits	samples;
		VkImageTiling			tiling;
		VkImageUsageFlags		usage;
		VkImageLayout			initialLayout;

		Parameters (VkImageType				imageType_,
					VkFormat				format_,
					VkExtent3D				extent_,
					deUint32				mipLevels_,
					deUint32				arraySize_,
					VkSampleCountFlagBits	samples_,
					VkImageTiling			tiling_,
					VkImageUsageFlags		usage_,
					VkImageLayout			initialLayout_)
			: imageType		(imageType_)
			, format		(format_)
			, extent		(extent_)
			, mipLevels		(mipLevels_)
			, arraySize		(arraySize_)
			, samples		(samples_)
			, tiling		(tiling_)
			, usage			(usage_)
			, initialLayout	(initialLayout_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkImage> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkImageCreateInfo		imageInfo	=
		{
			VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			DE_NULL,
			(VkImageCreateFlags)0,
			params.imageType,
			params.format,
			params.extent,
			params.mipLevels,
			params.arraySize,
			params.samples,
			params.tiling,
			params.usage,
			VK_SHARING_MODE_EXCLUSIVE,		// sharingMode
			1u,								// queueFamilyIndexCount
			&env.queueFamilyIndex,			// pQueueFamilyIndices
			params.initialLayout
		};

		return createImage(env.vkd, env.device, &imageInfo);
	}
};

struct ImageView
{
	typedef VkImageView Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		Image::Parameters		image;
		VkImageViewType			viewType;
		VkFormat				format;
		VkComponentMapping		components;
		VkImageSubresourceRange	subresourceRange;

		Parameters (const Image::Parameters&	image_,
					VkImageViewType				viewType_,
					VkFormat					format_,
					VkComponentMapping			components_,
					VkImageSubresourceRange		subresourceRange_)
			: image				(image_)
			, viewType			(viewType_)
			, format			(format_)
			, components		(components_)
			, subresourceRange	(subresourceRange_)
		{}
	};

	struct Resources
	{
		Dependency<Image>			image;
		Dependency<DeviceMemory>	memory;

		Resources (const Environment& env, const Parameters& params)
			: image	(env, params.image)
			, memory(env, getDeviceMemoryParameters(env, *image.object))
		{
			VK_CHECK(env.vkd.bindImageMemory(env.device, *image.object, *memory.object, 0));
		}
	};

	static Move<VkImageView> create (const Environment& env, const Resources& res, const Parameters& params)
	{
		const VkImageViewCreateInfo	imageViewInfo	=
		{
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			DE_NULL,
			(VkImageViewCreateFlags)0,
			*res.image.object,
			params.viewType,
			params.format,
			params.components,
			params.subresourceRange,
		};

		return createImageView(env.vkd, env.device, &imageViewInfo);
	}
};

struct Semaphore
{
	typedef VkSemaphore Type;

	enum { MAX_CONCURRENT = 100 };

	struct Parameters
	{
		VkSemaphoreCreateFlags	flags;

		Parameters (VkSemaphoreCreateFlags flags_)
			: flags(flags_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkSemaphore> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkSemaphoreCreateInfo	semaphoreInfo	=
		{
			VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			DE_NULL,
			params.flags
		};

		return createSemaphore(env.vkd, env.device, &semaphoreInfo);
	}
};

struct Fence
{
	typedef VkFence Type;

	enum { MAX_CONCURRENT = 100 };

	struct Parameters
	{
		VkFenceCreateFlags	flags;

		Parameters (VkFenceCreateFlags flags_)
			: flags(flags_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkFence> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkFenceCreateInfo	fenceInfo	=
		{
			VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			DE_NULL,
			params.flags
		};

		return createFence(env.vkd, env.device, &fenceInfo);
	}
};

struct Event
{
	typedef VkEvent Type;

	enum { MAX_CONCURRENT = 100 };

	struct Parameters
	{
		VkEventCreateFlags	flags;

		Parameters (VkEventCreateFlags flags_)
			: flags(flags_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkEvent> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkEventCreateInfo	eventInfo	=
		{
			VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
			DE_NULL,
			params.flags
		};

		return createEvent(env.vkd, env.device, &eventInfo);
	}
};

struct QueryPool
{
	typedef VkQueryPool Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkQueryType						queryType;
		deUint32						entryCount;
		VkQueryPipelineStatisticFlags	pipelineStatistics;

		Parameters (VkQueryType						queryType_,
					deUint32						entryCount_,
					VkQueryPipelineStatisticFlags	pipelineStatistics_)
			: queryType				(queryType_)
			, entryCount			(entryCount_)
			, pipelineStatistics	(pipelineStatistics_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkQueryPool> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkQueryPoolCreateInfo	queryPoolInfo	=
		{
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			DE_NULL,
			(VkQueryPoolCreateFlags)0,
			params.queryType,
			params.entryCount,
			params.pipelineStatistics
		};

		return createQueryPool(env.vkd, env.device, &queryPoolInfo);
	}
};

struct ShaderModule
{
	typedef VkShaderModule Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkShaderStageFlagBits	shaderStage;
		string					binaryName;

		Parameters (VkShaderStageFlagBits	shaderStage_,
					const std::string&		binaryName_)
			: shaderStage	(shaderStage_)
			, binaryName	(binaryName_)
		{}
	};

	struct Resources
	{
		const ProgramBinary&	binary;

		Resources (const Environment& env, const Parameters& params)
			: binary(env.programBinaries.get(params.binaryName))
		{}
	};

	static const char* getSource (VkShaderStageFlagBits stage)
	{
		switch (stage)
		{
			case VK_SHADER_STAGE_VERTEX_BIT:
				return "#version 310 es\n"
					   "layout(location = 0) in highp vec4 a_position;\n"
					   "void main () { gl_Position = a_position; }\n";

			case VK_SHADER_STAGE_FRAGMENT_BIT:
				return "#version 310 es\n"
					   "layout(location = 0) out mediump vec4 o_color;\n"
					   "void main () { o_color = vec4(1.0, 0.5, 0.25, 1.0); }";

			case VK_SHADER_STAGE_COMPUTE_BIT:
				return "#version 310 es\n"
					   "layout(binding = 0) buffer Input { highp uint dataIn[]; };\n"
					   "layout(binding = 1) buffer Output { highp uint dataOut[]; };\n"
					   "void main (void)\n"
					   "{\n"
					   "	dataOut[gl_GlobalInvocationID.x] = ~dataIn[gl_GlobalInvocationID.x];\n"
					   "}\n";

			default:
				DE_FATAL("Not implemented");
				return DE_NULL;
		}
	}

	static void initPrograms (SourceCollections& dst, Parameters params)
	{
		const char* const	source	= getSource(params.shaderStage);

		DE_ASSERT(source);

		dst.glslSources.add(params.binaryName)
			<< glu::ShaderSource(getGluShaderType(params.shaderStage), source);
	}

	static Move<VkShaderModule> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkShaderModuleCreateInfo	shaderModuleInfo	=
		{
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			DE_NULL,
			(VkShaderModuleCreateFlags)0,
			res.binary.getSize(),
			(const deUint32*)res.binary.getBinary(),
		};

		return createShaderModule(env.vkd, env.device, &shaderModuleInfo);
	}
};

struct PipelineCache
{
	typedef VkPipelineCache Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		Parameters (void) {}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkPipelineCache> create (const Environment& env, const Resources&, const Parameters&)
	{
		const VkPipelineCacheCreateInfo	pipelineCacheInfo	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
			DE_NULL,
			(VkPipelineCacheCreateFlags)0u,
			0u,								// initialDataSize
			DE_NULL,						// pInitialData
		};

		return createPipelineCache(env.vkd, env.device, &pipelineCacheInfo);
	}
};

struct Sampler
{
	typedef VkSampler Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkFilter				magFilter;
		VkFilter				minFilter;
		VkSamplerMipmapMode		mipmapMode;
		VkSamplerAddressMode	addressModeU;
		VkSamplerAddressMode	addressModeV;
		VkSamplerAddressMode	addressModeW;
		float					mipLodBias;
		float					maxAnisotropy;
		VkBool32				compareEnable;
		VkCompareOp				compareOp;
		float					minLod;
		float					maxLod;
		VkBorderColor			borderColor;
		VkBool32				unnormalizedCoordinates;

		// \todo [2015-09-17 pyry] Other configurations
		Parameters (void)
			: magFilter					(VK_FILTER_NEAREST)
			, minFilter					(VK_FILTER_NEAREST)
			, mipmapMode				(VK_SAMPLER_MIPMAP_MODE_BASE)
			, addressModeU				(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			, addressModeV				(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			, addressModeW				(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			, mipLodBias				(0.0f)
			, maxAnisotropy				(0.0f)
			, compareEnable				(VK_FALSE)
			, compareOp					(VK_COMPARE_OP_ALWAYS)
			, minLod					(-1000.f)
			, maxLod					(+1000.f)
			, borderColor				(VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK)
			, unnormalizedCoordinates	(VK_FALSE)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkSampler> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkSamplerCreateInfo	samplerInfo	=
		{
			VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			DE_NULL,
			(VkSamplerCreateFlags)0,
			params.magFilter,
			params.minFilter,
			params.mipmapMode,
			params.addressModeU,
			params.addressModeV,
			params.addressModeW,
			params.mipLodBias,
			params.maxAnisotropy,
			params.compareEnable,
			params.compareOp,
			params.minLod,
			params.maxLod,
			params.borderColor,
			params.unnormalizedCoordinates
		};

		return createSampler(env.vkd, env.device, &samplerInfo);
	}
};

struct DescriptorSetLayout
{
	typedef VkDescriptorSetLayout Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		struct Binding
		{
			deUint32			binding;
			VkDescriptorType	descriptorType;
			deUint32			descriptorCount;
			VkShaderStageFlags	stageFlags;
			bool				useImmutableSampler;

			Binding (deUint32			binding_,
					 VkDescriptorType	descriptorType_,
					 deUint32			descriptorCount_,
					 VkShaderStageFlags	stageFlags_,
					 bool				useImmutableSampler_)
				: binding				(binding_)
				, descriptorType		(descriptorType_)
				, descriptorCount		(descriptorCount_)
				, stageFlags			(stageFlags_)
				, useImmutableSampler	(useImmutableSampler_)
			{}

			Binding (void) {}
		};

		vector<Binding>	bindings;

		Parameters (const vector<Binding>& bindings_)
			: bindings(bindings_)
		{}

		static Parameters empty (void)
		{
			return Parameters(vector<Binding>());
		}

		static Parameters single (deUint32				binding,
								  VkDescriptorType		descriptorType,
								  deUint32				descriptorCount,
								  VkShaderStageFlags	stageFlags,
								  bool					useImmutableSampler = false)
		{
			vector<Binding> bindings;
			bindings.push_back(Binding(binding, descriptorType, descriptorCount, stageFlags, useImmutableSampler));
			return Parameters(bindings);
		}
	};

	struct Resources
	{
		vector<VkDescriptorSetLayoutBinding>	bindings;
		MovePtr<Dependency<Sampler> >			immutableSampler;
		vector<VkSampler>						immutableSamplersPtr;

		Resources (const Environment& env, const Parameters& params)
		{
			// Create immutable sampler if needed
			for (vector<Parameters::Binding>::const_iterator cur = params.bindings.begin(); cur != params.bindings.end(); cur++)
			{
				if (cur->useImmutableSampler && !immutableSampler)
				{
					immutableSampler = de::newMovePtr<Dependency<Sampler> >(env, Sampler::Parameters());

					if (cur->useImmutableSampler && immutableSamplersPtr.size() < (size_t)cur->descriptorCount)
						immutableSamplersPtr.resize(cur->descriptorCount, *immutableSampler->object);
				}
			}

			for (vector<Parameters::Binding>::const_iterator cur = params.bindings.begin(); cur != params.bindings.end(); cur++)
			{
				const VkDescriptorSetLayoutBinding	binding	=
				{
					cur->binding,
					cur->descriptorType,
					cur->descriptorCount,
					cur->stageFlags,
					(cur->useImmutableSampler ? &immutableSamplersPtr[0] : DE_NULL)
				};

				bindings.push_back(binding);
			}
		}
	};

	static Move<VkDescriptorSetLayout> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkDescriptorSetLayoutCreateInfo	descriptorSetLayoutInfo	=
		{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			DE_NULL,
			(VkDescriptorSetLayoutCreateFlags)0,
			(deUint32)res.bindings.size(),
			(res.bindings.empty() ? DE_NULL : &res.bindings[0])
		};

		return createDescriptorSetLayout(env.vkd, env.device, &descriptorSetLayoutInfo);
	}
};

struct PipelineLayout
{
	typedef VkPipelineLayout Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		vector<DescriptorSetLayout::Parameters>	descriptorSetLayouts;
		vector<VkPushConstantRange>				pushConstantRanges;

		Parameters (void) {}

		static Parameters empty (void)
		{
			return Parameters();
		}

		static Parameters singleDescriptorSet (const DescriptorSetLayout::Parameters& descriptorSetLayout)
		{
			Parameters params;
			params.descriptorSetLayouts.push_back(descriptorSetLayout);
			return params;
		}
	};

	struct Resources
	{
		typedef SharedPtr<Dependency<DescriptorSetLayout> >	DescriptorSetLayoutDepSp;
		typedef vector<DescriptorSetLayoutDepSp>			DescriptorSetLayouts;

		DescriptorSetLayouts			descriptorSetLayouts;
		vector<VkDescriptorSetLayout>	pSetLayouts;

		Resources (const Environment& env, const Parameters& params)
		{
			for (vector<DescriptorSetLayout::Parameters>::const_iterator dsParams = params.descriptorSetLayouts.begin();
				 dsParams != params.descriptorSetLayouts.end();
				 ++dsParams)
			{
				descriptorSetLayouts.push_back(DescriptorSetLayoutDepSp(new Dependency<DescriptorSetLayout>(env, *dsParams)));
				pSetLayouts.push_back(*descriptorSetLayouts.back()->object);
			}
		}
	};

	static Move<VkPipelineLayout> create (const Environment& env, const Resources& res, const Parameters& params)
	{
		const VkPipelineLayoutCreateInfo	pipelineLayoutInfo	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			DE_NULL,
			(VkPipelineLayoutCreateFlags)0,
			(deUint32)res.pSetLayouts.size(),
			(res.pSetLayouts.empty() ? DE_NULL : &res.pSetLayouts[0]),
			(deUint32)params.pushConstantRanges.size(),
			(params.pushConstantRanges.empty() ? DE_NULL : &params.pushConstantRanges[0]),
		};

		return createPipelineLayout(env.vkd, env.device, &pipelineLayoutInfo);
	}
};

struct RenderPass
{
	typedef VkRenderPass Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	// \todo [2015-09-17 pyry] More interesting configurations
	struct Parameters
	{
		Parameters (void) {}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkRenderPass> create (const Environment& env, const Resources&, const Parameters&)
	{
		const VkAttachmentDescription	attachments[]		=
		{
			{
				(VkAttachmentDescriptionFlags)0,
				VK_FORMAT_R8G8B8A8_UNORM,
				VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			},
			{
				(VkAttachmentDescriptionFlags)0,
				VK_FORMAT_D16_UNORM,
				VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			}
		};
		const VkAttachmentReference		colorAttachments[]	=
		{
			{
				0u,											// attachment
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			}
		};
		const VkAttachmentReference		dsAttachment		=
		{
			1u,											// attachment
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		};
		const VkSubpassDescription		subpasses[]			=
		{
			{
				(VkSubpassDescriptionFlags)0,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				0u,											// inputAttachmentCount
				DE_NULL,									// pInputAttachments
				DE_LENGTH_OF_ARRAY(colorAttachments),
				colorAttachments,
				DE_NULL,									// pResolveAttachments
				&dsAttachment,
				0u,											// preserveAttachmentCount
				DE_NULL,									// pPreserveAttachments
			}
		};
		const VkRenderPassCreateInfo	renderPassInfo		=
		{
			VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			DE_NULL,
			(VkRenderPassCreateFlags)0,
			DE_LENGTH_OF_ARRAY(attachments),
			attachments,
			DE_LENGTH_OF_ARRAY(subpasses),
			subpasses,
			0u,												// dependencyCount
			DE_NULL											// pDependencies
		};

		return createRenderPass(env.vkd, env.device, &renderPassInfo);
	}
};

struct GraphicsPipeline
{
	typedef VkPipeline Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	// \todo [2015-09-17 pyry] More interesting configurations
	struct Parameters
	{
		Parameters (void) {}
	};

	struct Resources
	{
		Dependency<ShaderModule>	vertexShader;
		Dependency<ShaderModule>	fragmentShader;
		Dependency<PipelineLayout>	layout;
		Dependency<RenderPass>		renderPass;
		Dependency<PipelineCache>	pipelineCache;

		Resources (const Environment& env, const Parameters&)
			: vertexShader		(env, ShaderModule::Parameters(VK_SHADER_STAGE_VERTEX_BIT, "vert"))
			, fragmentShader	(env, ShaderModule::Parameters(VK_SHADER_STAGE_FRAGMENT_BIT, "frag"))
			, layout			(env, PipelineLayout::Parameters::singleDescriptorSet(
										DescriptorSetLayout::Parameters::single(0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, true)))
			, renderPass		(env, RenderPass::Parameters())
			, pipelineCache		(env, PipelineCache::Parameters())
		{}
	};

	static void initPrograms (SourceCollections& dst, Parameters)
	{
		ShaderModule::initPrograms(dst, ShaderModule::Parameters(VK_SHADER_STAGE_VERTEX_BIT, "vert"));
		ShaderModule::initPrograms(dst, ShaderModule::Parameters(VK_SHADER_STAGE_FRAGMENT_BIT, "frag"));
	}

	static Move<VkPipeline> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkPipelineShaderStageCreateInfo			stages[]			=
		{
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				DE_NULL,
				(VkPipelineShaderStageCreateFlags)0,
				VK_SHADER_STAGE_VERTEX_BIT,
				*res.vertexShader.object,
				"main",
				DE_NULL,							// pSpecializationInfo
			},
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				DE_NULL,
				(VkPipelineShaderStageCreateFlags)0,
				VK_SHADER_STAGE_FRAGMENT_BIT,
				*res.fragmentShader.object,
				"main",
				DE_NULL,							// pSpecializationInfo
			}
		};
		const VkVertexInputBindingDescription			vertexBindings[]	=
		{
			{
				0u,									// binding
				16u,								// stride
				VK_VERTEX_INPUT_RATE_VERTEX
			}
		};
		const VkVertexInputAttributeDescription			vertexAttribs[]		=
		{
			{
				0u,									// location
				0u,									// binding
				VK_FORMAT_R32G32B32A32_SFLOAT,
				0u,									// offset
			}
		};
		const VkPipelineVertexInputStateCreateInfo		vertexInputState	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineVertexInputStateCreateFlags)0,
			DE_LENGTH_OF_ARRAY(vertexBindings),
			vertexBindings,
			DE_LENGTH_OF_ARRAY(vertexAttribs),
			vertexAttribs
		};
		const VkPipelineInputAssemblyStateCreateInfo	inputAssemblyState	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineInputAssemblyStateCreateFlags)0,
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			VK_FALSE								// primitiveRestartEnable
		};
		const VkViewport								viewports[]			=
		{
			{ 0.0f, 0.0f, 64.f, 64.f, 0.0f, 1.0f }
		};
		const VkRect2D									scissors[]			=
		{
			{ { 0, 0 }, { 64, 64 } }
		};
		const VkPipelineViewportStateCreateInfo			viewportState		=
		{
			VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineViewportStateCreateFlags)0,
			DE_LENGTH_OF_ARRAY(viewports),
			viewports,
			DE_LENGTH_OF_ARRAY(scissors),
			scissors,
		};
		const VkPipelineRasterizationStateCreateInfo	rasterState			=
		{
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineRasterizationStateCreateFlags)0,
			VK_TRUE,								// depthClampEnable
			VK_FALSE,								// rasterizerDiscardEnable
			VK_POLYGON_MODE_FILL,
			VK_CULL_MODE_BACK_BIT,
			VK_FRONT_FACE_COUNTER_CLOCKWISE,
			VK_FALSE,								// depthBiasEnable
			0.0f,									// depthBiasConstantFactor
			0.0f,									// depthBiasClamp
			0.0f,									// depthBiasSlopeFactor
			1.0f,									// lineWidth
		};
		const VkPipelineMultisampleStateCreateInfo		multisampleState	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineMultisampleStateCreateFlags)0,
			VK_SAMPLE_COUNT_1_BIT,
			VK_FALSE,								// sampleShadingEnable
			1.0f,									// minSampleShading
			DE_NULL,								// pSampleMask
			VK_FALSE,								// alphaToCoverageEnable
			VK_FALSE,								// alphaToOneEnable
		};
		const VkPipelineDepthStencilStateCreateInfo		depthStencilState	=
		{
			VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineDepthStencilStateCreateFlags)0,
			VK_TRUE,								// depthTestEnable
			VK_TRUE,								// depthWriteEnable
			VK_COMPARE_OP_LESS,						// depthCompareOp
			VK_FALSE,								// depthBoundsTestEnable
			VK_FALSE,								// stencilTestEnable
			{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0u, 0u, 0u },
			{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0u, 0u, 0u },
			-1.0f,									// minDepthBounds
			+1.0f,									// maxDepthBounds
		};
		const VkPipelineColorBlendAttachmentState		colorBlendAttState[]=
		{
			{
				VK_FALSE,							// blendEnable
				VK_BLEND_FACTOR_ONE,
				VK_BLEND_FACTOR_ZERO,
				VK_BLEND_OP_ADD,
				VK_BLEND_FACTOR_ONE,
				VK_BLEND_FACTOR_ZERO,
				VK_BLEND_OP_ADD,
				VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT
			}
		};
		const VkPipelineColorBlendStateCreateInfo		colorBlendState		=
		{
			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineColorBlendStateCreateFlags)0,
			VK_FALSE,								// logicOpEnable
			VK_LOGIC_OP_COPY,
			DE_LENGTH_OF_ARRAY(colorBlendAttState),
			colorBlendAttState,
			{ 0.0f, 0.0f, 0.0f, 0.0f }				// blendConstants
		};
		const VkPipelineDynamicStateCreateInfo			dynamicState		=
		{
			VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			DE_NULL,
			(VkPipelineDynamicStateCreateFlags)0,
			0u,										// dynamicStateCount
			DE_NULL,								// pDynamicStates
		};
		const VkGraphicsPipelineCreateInfo				pipelineInfo		=
		{
			VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			DE_NULL,
			(VkPipelineCreateFlags)0,
			DE_LENGTH_OF_ARRAY(stages),
			stages,
			&vertexInputState,
			&inputAssemblyState,
			DE_NULL,								// pTessellationState
			&viewportState,
			&rasterState,
			&multisampleState,
			&depthStencilState,
			&colorBlendState,
			&dynamicState,
			*res.layout.object,
			*res.renderPass.object,
			0u,										// subpass
			(VkPipeline)0,							// basePipelineHandle
			0,										// basePipelineIndex
		};

		return createGraphicsPipeline(env.vkd, env.device, *res.pipelineCache.object, &pipelineInfo);
	}
};

struct ComputePipeline
{
	typedef VkPipeline Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	// \todo [2015-09-17 pyry] More interesting configurations
	struct Parameters
	{
		Parameters (void) {}
	};

	struct Resources
	{
		Dependency<ShaderModule>	shaderModule;
		Dependency<PipelineLayout>	layout;
		Dependency<PipelineCache>	pipelineCache;

		static DescriptorSetLayout::Parameters getDescriptorSetLayout (void)
		{
			typedef DescriptorSetLayout::Parameters::Binding Binding;

			vector<Binding> bindings;

			bindings.push_back(Binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, false));
			bindings.push_back(Binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, false));

			return DescriptorSetLayout::Parameters(bindings);
		}

		Resources (const Environment& env, const Parameters&)
			: shaderModule		(env, ShaderModule::Parameters(VK_SHADER_STAGE_COMPUTE_BIT, "comp"))
			, layout			(env, PipelineLayout::Parameters::singleDescriptorSet(getDescriptorSetLayout()))
			, pipelineCache		(env, PipelineCache::Parameters())
		{}
	};

	static void initPrograms (SourceCollections& dst, Parameters)
	{
		ShaderModule::initPrograms(dst, ShaderModule::Parameters(VK_SHADER_STAGE_COMPUTE_BIT, "comp"));
	}

	static Move<VkPipeline> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkComputePipelineCreateInfo	pipelineInfo	=
		{
			VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			DE_NULL,
			(VkPipelineCreateFlags)0,
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				DE_NULL,
				(VkPipelineShaderStageCreateFlags)0,
				VK_SHADER_STAGE_COMPUTE_BIT,
				*res.shaderModule.object,
				"main",
				DE_NULL					// pSpecializationInfo
			},
			*res.layout.object,
			(VkPipeline)0,				// basePipelineHandle
			0u,							// basePipelineIndex
		};

		return createComputePipeline(env.vkd, env.device, *res.pipelineCache.object, &pipelineInfo);
	}
};

struct DescriptorPool
{
	typedef VkDescriptorPool Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkDescriptorPoolCreateFlags		flags;
		deUint32						maxSets;
		vector<VkDescriptorPoolSize>	poolSizes;

		Parameters (VkDescriptorPoolCreateFlags				flags_,
					deUint32								maxSets_,
					const vector<VkDescriptorPoolSize>&		poolSizes_)
			: flags		(flags_)
			, maxSets	(maxSets_)
			, poolSizes	(poolSizes_)
		{}

		static Parameters singleType (VkDescriptorPoolCreateFlags	flags,
									  deUint32						maxSets,
									  VkDescriptorType				type,
									  deUint32						count)
		{
			vector<VkDescriptorPoolSize> poolSizes;
			poolSizes.push_back(makeDescriptorPoolSize(type, count));
			return Parameters(flags, maxSets, poolSizes);
		}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkDescriptorPool> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkDescriptorPoolCreateInfo	descriptorPoolInfo	=
		{
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			DE_NULL,
			params.flags,
			params.maxSets,
			(deUint32)params.poolSizes.size(),
			(params.poolSizes.empty() ? DE_NULL : &params.poolSizes[0])
		};

		return createDescriptorPool(env.vkd, env.device, &descriptorPoolInfo);
	}
};

struct DescriptorSet
{
	typedef VkDescriptorSet Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		DescriptorSetLayout::Parameters	descriptorSetLayout;

		Parameters (const DescriptorSetLayout::Parameters& descriptorSetLayout_)
			: descriptorSetLayout(descriptorSetLayout_)
		{}
	};

	struct Resources
	{
		Dependency<DescriptorPool>		descriptorPool;
		Dependency<DescriptorSetLayout>	descriptorSetLayout;

		static vector<VkDescriptorPoolSize> computePoolSizes (const DescriptorSetLayout::Parameters& layout)
		{
			deUint32						countByType[VK_DESCRIPTOR_TYPE_LAST];
			vector<VkDescriptorPoolSize>	typeCounts;

			std::fill(DE_ARRAY_BEGIN(countByType), DE_ARRAY_END(countByType), 0u);

			for (vector<DescriptorSetLayout::Parameters::Binding>::const_iterator cur = layout.bindings.begin();
				 cur != layout.bindings.end();
				 ++cur)
			{
				DE_ASSERT((deUint32)cur->descriptorType < VK_DESCRIPTOR_TYPE_LAST);
				countByType[cur->descriptorType] += cur->descriptorCount;
			}

			for (deUint32 type = 0; type < VK_DESCRIPTOR_TYPE_LAST; ++type)
			{
				if (countByType[type] > 0)
					typeCounts.push_back(makeDescriptorPoolSize((VkDescriptorType)type, countByType[type]));
			}

			return typeCounts;
		}

		Resources (const Environment& env, const Parameters& params)
			: descriptorPool		(env, DescriptorPool::Parameters(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, env.maxResourceConsumers, computePoolSizes(params.descriptorSetLayout)))
			, descriptorSetLayout	(env, params.descriptorSetLayout)
		{
		}
	};

	static Move<VkDescriptorSet> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkDescriptorSetAllocateInfo	allocateInfo	=
		{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			DE_NULL,
			*res.descriptorPool.object,
			1u,
			&res.descriptorSetLayout.object.get(),
		};

		return allocateDescriptorSet(env.vkd, env.device, &allocateInfo);
	}
};

struct Framebuffer
{
	typedef VkFramebuffer Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		Parameters (void)
		{}
	};

	struct Resources
	{
		Dependency<ImageView>	colorAttachment;
		Dependency<ImageView>	depthStencilAttachment;
		Dependency<RenderPass>	renderPass;

		Resources (const Environment& env, const Parameters&)
			: colorAttachment			(env, ImageView::Parameters(Image::Parameters(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
																					  makeExtent3D(256, 256, 1),
																					  1u, 1u,
																					  VK_SAMPLE_COUNT_1_BIT,
																					  VK_IMAGE_TILING_OPTIMAL,
																					  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
																					  VK_IMAGE_LAYOUT_UNDEFINED),
																		 VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
																		 makeComponentMappingRGBA(),
																		 makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u)))
			, depthStencilAttachment	(env, ImageView::Parameters(Image::Parameters(VK_IMAGE_TYPE_2D, VK_FORMAT_D16_UNORM,
																					  makeExtent3D(256, 256, 1),
																					  1u, 1u,
																					  VK_SAMPLE_COUNT_1_BIT,
																					  VK_IMAGE_TILING_OPTIMAL,
																					  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
																					  VK_IMAGE_LAYOUT_UNDEFINED),
																		 VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D16_UNORM,
																		 makeComponentMappingRGBA(),
																		 makeImageSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u)))
			, renderPass				(env, RenderPass::Parameters())
		{}
	};

	static Move<VkFramebuffer> create (const Environment& env, const Resources& res, const Parameters&)
	{
		const VkImageView				attachments[]	=
		{
			*res.colorAttachment.object,
			*res.depthStencilAttachment.object,
		};
		const VkFramebufferCreateInfo	framebufferInfo	=
		{
			VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			DE_NULL,
			(VkFramebufferCreateFlags)0,
			*res.renderPass.object,
			(deUint32)DE_LENGTH_OF_ARRAY(attachments),
			attachments,
			256u,										// width
			256u,										// height
			1u											// layers
		};

		return createFramebuffer(env.vkd, env.device, &framebufferInfo);
	}
};

struct CommandPool
{
	typedef VkCommandPool Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		VkCommandPoolCreateFlags	flags;

		Parameters (VkCommandPoolCreateFlags flags_)
			: flags(flags_)
		{}
	};

	struct Resources
	{
		Resources (const Environment&, const Parameters&) {}
	};

	static Move<VkCommandPool> create (const Environment& env, const Resources&, const Parameters& params)
	{
		const VkCommandPoolCreateInfo	cmdPoolInfo	=
		{
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			DE_NULL,
			params.flags,
			env.queueFamilyIndex,
		};

		return createCommandPool(env.vkd, env.device, &cmdPoolInfo);
	}
};

struct CommandBuffer
{
	typedef VkCommandBuffer Type;

	enum { MAX_CONCURRENT = DEFAULT_MAX_CONCURRENT_OBJECTS };

	struct Parameters
	{
		CommandPool::Parameters		commandPool;
		VkCommandBufferLevel		level;

		Parameters (const CommandPool::Parameters&	commandPool_,
					VkCommandBufferLevel			level_)
			: commandPool	(commandPool_)
			, level			(level_)
		{}
	};

	struct Resources
	{
		Dependency<CommandPool>	commandPool;

		Resources (const Environment& env, const Parameters& params)
			: commandPool(env, params.commandPool)
		{}
	};

	static Move<VkCommandBuffer> create (const Environment& env, const Resources& res, const Parameters& params)
	{
		const VkCommandBufferAllocateInfo	cmdBufferInfo	=
		{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			DE_NULL,
			*res.commandPool.object,
			params.level,
			1,							// bufferCount
		};

		return allocateCommandBuffer(env.vkd, env.device, &cmdBufferInfo);
	}
};

// Test cases

template<typename Object>
tcu::TestStatus createSingleTest (Context& context, typename Object::Parameters params)
{
	const Environment					env	(context, 1u);
	const typename Object::Resources	res	(env, params);

	{
		Unique<typename Object::Type>	obj	(Object::create(env, res, params));
	}

	return tcu::TestStatus::pass("Ok");
}

template<typename Object>
tcu::TestStatus createMultipleUniqueResourcesTest (Context& context, typename Object::Parameters params)
{
	const Environment					env		(context, 1u);
	const typename Object::Resources	res0	(env, params);
	const typename Object::Resources	res1	(env, params);
	const typename Object::Resources	res2	(env, params);
	const typename Object::Resources	res3	(env, params);

	{
		Unique<typename Object::Type>	obj0	(Object::create(env, res0, params));
		Unique<typename Object::Type>	obj1	(Object::create(env, res1, params));
		Unique<typename Object::Type>	obj2	(Object::create(env, res2, params));
		Unique<typename Object::Type>	obj3	(Object::create(env, res3, params));
	}

	return tcu::TestStatus::pass("Ok");
}

template<typename Object>
tcu::TestStatus createMultipleSharedResourcesTest (Context& context, typename Object::Parameters params)
{
	const Environment					env	(context, 4u);
	const typename Object::Resources	res	(env, params);

	{
		Unique<typename Object::Type>	obj0	(Object::create(env, res, params));
		Unique<typename Object::Type>	obj1	(Object::create(env, res, params));
		Unique<typename Object::Type>	obj2	(Object::create(env, res, params));
		Unique<typename Object::Type>	obj3	(Object::create(env, res, params));
	}

	return tcu::TestStatus::pass("Ok");
}

template<typename Object>
tcu::TestStatus createMaxConcurrentTest (Context& context, typename Object::Parameters params)
{
	typedef Unique<typename Object::Type>	UniqueObject;
	typedef SharedPtr<UniqueObject>			ObjectPtr;

	const deUint32						numObjects	= Object::MAX_CONCURRENT;
	const Environment					env			(context, numObjects);
	const typename Object::Resources	res			(env, params);
	vector<ObjectPtr>					objects		(numObjects);

	context.getTestContext().getLog()
		<< TestLog::Message << "Creating " << numObjects << " " << getTypeName<typename Object::Type>() << "s" << TestLog::EndMessage;

	for (deUint32 ndx = 0; ndx < numObjects; ndx++)
		objects[ndx] = ObjectPtr(new UniqueObject(Object::create(env, res, params)));

	objects.clear();

	return tcu::TestStatus::pass("Ok");
}

template<typename Object>
class CreateThread : public ThreadGroupThread
{
public:
	CreateThread (const Environment& env, const typename Object::Resources& resources, const typename Object::Parameters& params)
		: m_env			(env)
		, m_resources	(resources)
		, m_params		(params)
	{}

	void runThread (void)
	{
		const int	numIters			= 100;
		const int	itersBetweenSyncs	= 20;

		for (int iterNdx = 0; iterNdx < numIters; iterNdx++)
		{
			// Sync every Nth iteration to make entering driver at the same time more likely
			if ((iterNdx % itersBetweenSyncs) == 0)
				barrier();

			{
				Unique<typename Object::Type>	obj	(Object::create(m_env, m_resources, m_params));
			}
		}
	}

private:
	const Environment&					m_env;
	const typename Object::Resources&	m_resources;
	const typename Object::Parameters&	m_params;
};

template<typename Object>
tcu::TestStatus multithreadedCreateSharedResourcesTest (Context& context, typename Object::Parameters params)
{
	const deUint32						numThreads	= getDefaultTestThreadCount();
	const Environment					env			(context, numThreads);
	const typename Object::Resources	res			(env, params);
	ThreadGroup							threads;

	for (deUint32 ndx = 0; ndx < numThreads; ndx++)
		threads.add(MovePtr<ThreadGroupThread>(new CreateThread<Object>(env, res, params)));

	return threads.run();
}

template<typename Object>
tcu::TestStatus multithreadedCreatePerThreadResourcesTest (Context& context, typename Object::Parameters params)
{
	typedef SharedPtr<typename Object::Resources>	ResPtr;

	const deUint32		numThreads	= getDefaultTestThreadCount();
	const Environment	env			(context, 1u);
	vector<ResPtr>		resources	(numThreads);
	ThreadGroup			threads;

	for (deUint32 ndx = 0; ndx < numThreads; ndx++)
	{
		resources[ndx] = ResPtr(new typename Object::Resources(env, params));
		threads.add(MovePtr<ThreadGroupThread>(new CreateThread<Object>(env, *resources[ndx], params)));
	}

	return threads.run();
}

struct EnvClone
{
	Device::Resources	deviceRes;
	Unique<VkDevice>	device;
	DeviceDriver		vkd;
	Environment			env;

	EnvClone (const Environment& parent, const Device::Parameters& deviceParams, deUint32 maxResourceConsumers)
		: deviceRes	(parent, deviceParams)
		, device	(Device::create(parent, deviceRes, deviceParams))
		, vkd		(deviceRes.vki, *device)
		, env		(parent.vkp, vkd, *device, deviceRes.queueFamilyIndex, parent.programBinaries, maxResourceConsumers)
	{
	}
};

template<typename Object>
tcu::TestStatus multithreadedCreatePerThreadDeviceTest (Context& context, typename Object::Parameters params)
{
	typedef SharedPtr<EnvClone>						EnvPtr;
	typedef SharedPtr<typename Object::Resources>	ResPtr;

	const deUint32				numThreads		= getDefaultTestThreadCount();
	const Device::Parameters	deviceParams	(context.getTestContext().getCommandLine().getVKDeviceId()-1u, VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);
	const Environment			sharedEnv		(context, numThreads);			// For creating Device's
	vector<EnvPtr>				perThreadEnv	(numThreads);
	vector<ResPtr>				resources		(numThreads);
	ThreadGroup					threads;

	for (deUint32 ndx = 0; ndx < numThreads; ndx++)
	{
		perThreadEnv[ndx]	= EnvPtr(new EnvClone(sharedEnv, deviceParams, 1u));
		resources[ndx]		= ResPtr(new typename Object::Resources(perThreadEnv[ndx]->env, params));

		threads.add(MovePtr<ThreadGroupThread>(new CreateThread<Object>(perThreadEnv[ndx]->env, *resources[ndx], params)));
	}

	return threads.run();
}

// Utilities for creating groups

template<typename Object>
struct NamedParameters
{
	const char*						name;
	typename Object::Parameters		parameters;
};

template<typename Object>
struct CaseDescription
{
	typename FunctionInstance1<typename Object::Parameters>::Function	function;
	const NamedParameters<Object>*										paramsBegin;
	const NamedParameters<Object>*										paramsEnd;
};

#define EMPTY_CASE_DESC(OBJECT)	\
	{ (FunctionInstance1<OBJECT::Parameters>::Function)DE_NULL, DE_NULL, DE_NULL }

#define CASE_DESC(FUNCTION, CASES)	\
	{ FUNCTION, DE_ARRAY_BEGIN(CASES), DE_ARRAY_END(CASES)	}

struct CaseDescriptions
{
	CaseDescription<Instance>				instance;
	CaseDescription<Device>					device;
	CaseDescription<DeviceMemory>			deviceMemory;
	CaseDescription<Buffer>					buffer;
	CaseDescription<BufferView>				bufferView;
	CaseDescription<Image>					image;
	CaseDescription<ImageView>				imageView;
	CaseDescription<Semaphore>				semaphore;
	CaseDescription<Event>					event;
	CaseDescription<Fence>					fence;
	CaseDescription<QueryPool>				queryPool;
	CaseDescription<ShaderModule>			shaderModule;
	CaseDescription<PipelineCache>			pipelineCache;
	CaseDescription<PipelineLayout>			pipelineLayout;
	CaseDescription<RenderPass>				renderPass;
	CaseDescription<GraphicsPipeline>		graphicsPipeline;
	CaseDescription<ComputePipeline>		computePipeline;
	CaseDescription<DescriptorSetLayout>	descriptorSetLayout;
	CaseDescription<Sampler>				sampler;
	CaseDescription<DescriptorPool>			descriptorPool;
	CaseDescription<DescriptorSet>			descriptorSet;
	CaseDescription<Framebuffer>			framebuffer;
	CaseDescription<CommandPool>			commandPool;
	CaseDescription<CommandBuffer>			commandBuffer;
};

template<typename Object>
void addCases (const MovePtr<tcu::TestCaseGroup>& group, const CaseDescription<Object>& cases)
{
	for (const NamedParameters<Object>* cur = cases.paramsBegin; cur != cases.paramsEnd; ++cur)
		addFunctionCase(group.get(), cur->name, "", cases.function, cur->parameters);
}

template<typename Object>
void addCasesWithProgs (const MovePtr<tcu::TestCaseGroup>& group, const CaseDescription<Object>& cases)
{
	for (const NamedParameters<Object>* cur = cases.paramsBegin; cur != cases.paramsEnd; ++cur)
		addFunctionCaseWithPrograms(group.get(), cur->name, "", Object::initPrograms, cases.function, cur->parameters);
}

tcu::TestCaseGroup* createGroup (tcu::TestContext& testCtx, const char* name, const char* desc, const CaseDescriptions& cases)
{
	MovePtr<tcu::TestCaseGroup>	group	(new tcu::TestCaseGroup(testCtx, name, desc));

	addCases			(group, cases.instance);
	addCases			(group, cases.device);
	addCases			(group, cases.deviceMemory);
	addCases			(group, cases.buffer);
	addCases			(group, cases.bufferView);
	addCases			(group, cases.image);
	addCases			(group, cases.imageView);
	addCases			(group, cases.semaphore);
	addCases			(group, cases.event);
	addCases			(group, cases.fence);
	addCases			(group, cases.queryPool);
	addCases			(group, cases.sampler);
	addCasesWithProgs	(group, cases.shaderModule);
	addCases			(group, cases.pipelineCache);
	addCases			(group, cases.pipelineLayout);
	addCases			(group, cases.renderPass);
	addCasesWithProgs	(group, cases.graphicsPipeline);
	addCasesWithProgs	(group, cases.computePipeline);
	addCases			(group, cases.descriptorSetLayout);
	addCases			(group, cases.descriptorPool);
	addCases			(group, cases.descriptorSet);
	addCases			(group, cases.framebuffer);
	addCases			(group, cases.commandPool);
	addCases			(group, cases.commandBuffer);

	return group.release();
}

} // anonymous

tcu::TestCaseGroup* createObjectManagementTests (tcu::TestContext& testCtx)
{
	MovePtr<tcu::TestCaseGroup>	objectMgmtTests	(new tcu::TestCaseGroup(testCtx, "object_management", "Object management tests"));

	const Image::Parameters		img1D			(VK_IMAGE_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, makeExtent3D(256,   1, 1), 1u,  4u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
	const Image::Parameters		img2D			(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, makeExtent3D( 64,  64, 1), 1u, 12u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
	const Image::Parameters		img3D			(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, makeExtent3D( 64,  64, 4), 1u,  1u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
	const ImageView::Parameters	imgView1D		(img1D, VK_IMAGE_VIEW_TYPE_1D,			img1D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u));
	const ImageView::Parameters	imgView1DArr	(img1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY,	img1D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 4u));
	const ImageView::Parameters	imgView2D		(img2D, VK_IMAGE_VIEW_TYPE_2D,			img2D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u));
	const ImageView::Parameters	imgView2DArr	(img2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,	img2D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 8u));
	const ImageView::Parameters	imgViewCube		(img2D, VK_IMAGE_VIEW_TYPE_CUBE,		img2D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 6u));
	const ImageView::Parameters	imgViewCubeArr	(img2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,	img2D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 12u));
	const ImageView::Parameters	imgView3D		(img3D, VK_IMAGE_VIEW_TYPE_3D,			img3D.format, makeComponentMappingRGBA(), makeImageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u));

	const DescriptorSetLayout::Parameters	singleUboDescLayout	= DescriptorSetLayout::Parameters::single(0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_VERTEX_BIT);

	static NamedParameters<Instance>				s_instanceCases[]			=
	{
		{ "instance",					Instance::Parameters() },
	};
	// \note Device index may change - must not be static
	const NamedParameters<Device>					s_deviceCases[]				=
	{
		{ "device",						Device::Parameters(testCtx.getCommandLine().getVKDeviceId()-1u, VK_QUEUE_GRAPHICS_BIT)	},
	};
	static const NamedParameters<DeviceMemory>			s_deviceMemCases[]				=
	{
		{ "device_memory_small",		DeviceMemory::Parameters(1024, 0u)	},
	};
	static const NamedParameters<Buffer>				s_bufferCases[]					=
	{
		{ "buffer_uniform_small",		Buffer::Parameters(1024u,			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),	},
		{ "buffer_uniform_large",		Buffer::Parameters(1024u*1024u*16u,	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),	},
		{ "buffer_storage_small",		Buffer::Parameters(1024u,			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),	},
		{ "buffer_storage_large",		Buffer::Parameters(1024u*1024u*16u,	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),	},
	};
	static const NamedParameters<BufferView>			s_bufferViewCases[]				=
	{
		{ "buffer_view_uniform_r8g8b8a8_unorm",	BufferView::Parameters(Buffer::Parameters(8192u, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT), VK_FORMAT_R8G8B8A8_UNORM, 0u, 4096u)	},
		{ "buffer_view_storage_r8g8b8a8_unorm",	BufferView::Parameters(Buffer::Parameters(8192u, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT), VK_FORMAT_R8G8B8A8_UNORM, 0u, 4096u)	},
	};
	static const NamedParameters<Image>					s_imageCases[]					=
	{
		{ "image_1d",					img1D		},
		{ "image_2d",					img2D		},
		{ "image_3d",					img3D		},
	};
	static const NamedParameters<ImageView>				s_imageViewCases[]				=
	{
		{ "image_view_1d",				imgView1D		},
		{ "image_view_1d_arr",			imgView1DArr	},
		{ "image_view_2d",				imgView2D		},
		{ "image_view_2d_arr",			imgView2DArr	},
		{ "image_view_cube",			imgViewCube		},
		{ "image_view_cube_arr",		imgViewCubeArr	},
		{ "image_view_3d",				imgView3D		},
	};
	static const NamedParameters<Semaphore>				s_semaphoreCases[]				=
	{
		{ "semaphore",					Semaphore::Parameters(0u),	}
	};
	static const NamedParameters<Event>					s_eventCases[]					=
	{
		{ "event",						Event::Parameters(0u)		}
	};
	static const NamedParameters<Fence>					s_fenceCases[]					=
	{
		{ "fence",						Fence::Parameters(0u)								},
		{ "fence_signaled",				Fence::Parameters(VK_FENCE_CREATE_SIGNALED_BIT)		}
	};
	static const NamedParameters<QueryPool>				s_queryPoolCases[]				=
	{
		{ "query_pool",					QueryPool::Parameters(VK_QUERY_TYPE_OCCLUSION, 1u, 0u)	}
	};
	static const NamedParameters<ShaderModule>			s_shaderModuleCases[]			=
	{
		{ "shader_module",				ShaderModule::Parameters(VK_SHADER_STAGE_COMPUTE_BIT, "test")	}
	};
	static const NamedParameters<PipelineCache>			s_pipelineCacheCases[]			=
	{
		{ "pipeline_cache",				PipelineCache::Parameters()		}
	};
	static const NamedParameters<PipelineLayout>		s_pipelineLayoutCases[]			=
	{
		{ "pipeline_layout_empty",		PipelineLayout::Parameters::empty()										},
		{ "pipeline_layout_single",		PipelineLayout::Parameters::singleDescriptorSet(singleUboDescLayout)	}
	};
	static const NamedParameters<RenderPass>			s_renderPassCases[]				=
	{
		{ "render_pass",				RenderPass::Parameters()		}
	};
	static const NamedParameters<GraphicsPipeline>		s_graphicsPipelineCases[]		=
	{
		{ "graphics_pipeline",			GraphicsPipeline::Parameters()	}
	};
	static const NamedParameters<ComputePipeline>		s_computePipelineCases[]		=
	{
		{ "compute_pipeline",			ComputePipeline::Parameters()	}
	};
	static const NamedParameters<DescriptorSetLayout>	s_descriptorSetLayoutCases[]	=
	{
		{ "descriptor_set_layout_empty",	DescriptorSetLayout::Parameters::empty()	},
		{ "descriptor_set_layout_single",	singleUboDescLayout							}
	};
	static const NamedParameters<Sampler>				s_samplerCases[]				=
	{
		{ "sampler",					Sampler::Parameters()	}
	};
	static const NamedParameters<DescriptorPool>		s_descriptorPoolCases[]			=
	{
		{ "descriptor_pool",						DescriptorPool::Parameters::singleType((VkDescriptorPoolCreateFlags)0,						4u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3u)	},
		{ "descriptor_pool_free_descriptor_set",	DescriptorPool::Parameters::singleType(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,	4u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3u)	}
	};
	static const NamedParameters<DescriptorSet>			s_descriptorSetCases[]			=
	{
		{ "descriptor_set",				DescriptorSet::Parameters(singleUboDescLayout)	}
	};
	static const NamedParameters<Framebuffer>			s_framebufferCases[]			=
	{
		{ "framebuffer",				Framebuffer::Parameters()	}
	};
	static const NamedParameters<CommandPool>			s_commandPoolCases[]			=
	{
		{ "command_pool",				CommandPool::Parameters((VkCommandPoolCreateFlags)0)			},
		{ "command_pool_transient",		CommandPool::Parameters(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)	}
	};
	static const NamedParameters<CommandBuffer>			s_commandBufferCases[]			=
	{
		{ "command_buffer_primary",		CommandBuffer::Parameters(CommandPool::Parameters((VkCommandPoolCreateFlags)0u), VK_COMMAND_BUFFER_LEVEL_PRIMARY)	},
		{ "command_buffer_secondary",	CommandBuffer::Parameters(CommandPool::Parameters((VkCommandPoolCreateFlags)0u), VK_COMMAND_BUFFER_LEVEL_SECONDARY)	}
	};

	static const CaseDescriptions	s_createSingleGroup	=
	{
		CASE_DESC(createSingleTest	<Instance>,					s_instanceCases),
		CASE_DESC(createSingleTest	<Device>,					s_deviceCases),
		CASE_DESC(createSingleTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(createSingleTest	<Buffer>,					s_bufferCases),
		CASE_DESC(createSingleTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(createSingleTest	<Image>,					s_imageCases),
		CASE_DESC(createSingleTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(createSingleTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(createSingleTest	<Event>,					s_eventCases),
		CASE_DESC(createSingleTest	<Fence>,					s_fenceCases),
		CASE_DESC(createSingleTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(createSingleTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(createSingleTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(createSingleTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(createSingleTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(createSingleTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(createSingleTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(createSingleTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(createSingleTest	<Sampler>,					s_samplerCases),
		CASE_DESC(createSingleTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(createSingleTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(createSingleTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(createSingleTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(createSingleTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "single", "Create single object", s_createSingleGroup));

	static const CaseDescriptions	s_createMultipleUniqueResourcesGroup	=
	{
		CASE_DESC(createMultipleUniqueResourcesTest	<Instance>,					s_instanceCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Device>,					s_deviceCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Buffer>,					s_bufferCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Image>,					s_imageCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Event>,					s_eventCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Fence>,					s_fenceCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Sampler>,					s_samplerCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(createMultipleUniqueResourcesTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "multiple_unique_resources", "Multiple objects with per-object unique resources", s_createMultipleUniqueResourcesGroup));

	static const CaseDescriptions	s_createMultipleSharedResourcesGroup	=
	{
		EMPTY_CASE_DESC(Instance), // No resources used
		CASE_DESC(createMultipleSharedResourcesTest	<Device>,					s_deviceCases),
		CASE_DESC(createMultipleSharedResourcesTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Buffer>,					s_bufferCases),
		CASE_DESC(createMultipleSharedResourcesTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Image>,					s_imageCases),
		CASE_DESC(createMultipleSharedResourcesTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Event>,					s_eventCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Fence>,					s_fenceCases),
		CASE_DESC(createMultipleSharedResourcesTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(createMultipleSharedResourcesTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(createMultipleSharedResourcesTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(createMultipleSharedResourcesTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(createMultipleSharedResourcesTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(createMultipleSharedResourcesTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(createMultipleSharedResourcesTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(createMultipleSharedResourcesTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Sampler>,					s_samplerCases),
		CASE_DESC(createMultipleSharedResourcesTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(createMultipleSharedResourcesTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(createMultipleSharedResourcesTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(createMultipleSharedResourcesTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(createMultipleSharedResourcesTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "multiple_shared_resources", "Multiple objects with shared resources", s_createMultipleSharedResourcesGroup));

	static const CaseDescriptions	s_createMaxConcurrentGroup	=
	{
		CASE_DESC(createMaxConcurrentTest	<Instance>,					s_instanceCases),
		CASE_DESC(createMaxConcurrentTest	<Device>,					s_deviceCases),
		CASE_DESC(createMaxConcurrentTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(createMaxConcurrentTest	<Buffer>,					s_bufferCases),
		CASE_DESC(createMaxConcurrentTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(createMaxConcurrentTest	<Image>,					s_imageCases),
		CASE_DESC(createMaxConcurrentTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(createMaxConcurrentTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(createMaxConcurrentTest	<Event>,					s_eventCases),
		CASE_DESC(createMaxConcurrentTest	<Fence>,					s_fenceCases),
		CASE_DESC(createMaxConcurrentTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(createMaxConcurrentTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(createMaxConcurrentTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(createMaxConcurrentTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(createMaxConcurrentTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(createMaxConcurrentTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(createMaxConcurrentTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(createMaxConcurrentTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(createMaxConcurrentTest	<Sampler>,					s_samplerCases),
		CASE_DESC(createMaxConcurrentTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(createMaxConcurrentTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(createMaxConcurrentTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(createMaxConcurrentTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(createMaxConcurrentTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "max_concurrent", "Maximum number of concurrently live objects", s_createMaxConcurrentGroup));

	static const CaseDescriptions	s_multithreadedCreatePerThreadDeviceGroup	=
	{
		EMPTY_CASE_DESC(Instance),	// Does not make sense
		EMPTY_CASE_DESC(Device),	// Does not make sense
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Buffer>,					s_bufferCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Image>,					s_imageCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Event>,					s_eventCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Fence>,					s_fenceCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Sampler>,					s_samplerCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(multithreadedCreatePerThreadDeviceTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "multithreaded_per_thread_device", "Multithreaded object construction with per-thread device ", s_multithreadedCreatePerThreadDeviceGroup));

	static const CaseDescriptions	s_multithreadedCreatePerThreadResourcesGroup	=
	{
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Instance>,					s_instanceCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Device>,					s_deviceCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Buffer>,					s_bufferCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Image>,					s_imageCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Event>,					s_eventCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Fence>,					s_fenceCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Sampler>,					s_samplerCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<DescriptorPool>,			s_descriptorPoolCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<DescriptorSet>,			s_descriptorSetCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<CommandPool>,				s_commandPoolCases),
		CASE_DESC(multithreadedCreatePerThreadResourcesTest	<CommandBuffer>,			s_commandBufferCases),
	};
	objectMgmtTests->addChild(createGroup(testCtx, "multithreaded_per_thread_resources", "Multithreaded object construction with per-thread resources", s_multithreadedCreatePerThreadResourcesGroup));

	static const CaseDescriptions	s_multithreadedCreateSharedResourcesGroup	=
	{
		EMPTY_CASE_DESC(Instance),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Device>,					s_deviceCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<DeviceMemory>,				s_deviceMemCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Buffer>,					s_bufferCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<BufferView>,				s_bufferViewCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Image>,					s_imageCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<ImageView>,				s_imageViewCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Semaphore>,				s_semaphoreCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Event>,					s_eventCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Fence>,					s_fenceCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<QueryPool>,				s_queryPoolCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<ShaderModule>,				s_shaderModuleCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<PipelineCache>,			s_pipelineCacheCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<PipelineLayout>,			s_pipelineLayoutCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<RenderPass>,				s_renderPassCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<GraphicsPipeline>,			s_graphicsPipelineCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<ComputePipeline>,			s_computePipelineCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<DescriptorSetLayout>,		s_descriptorSetLayoutCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Sampler>,					s_samplerCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<DescriptorPool>,			s_descriptorPoolCases),
		EMPTY_CASE_DESC(DescriptorSet),		// \note Needs per-thread DescriptorPool
		CASE_DESC(multithreadedCreateSharedResourcesTest	<Framebuffer>,				s_framebufferCases),
		CASE_DESC(multithreadedCreateSharedResourcesTest	<CommandPool>,				s_commandPoolCases),
		EMPTY_CASE_DESC(CommandBuffer),			// \note Needs per-thread CommandPool
	};
	objectMgmtTests->addChild(createGroup(testCtx, "multithreaded_shared_resources", "Multithreaded object construction with shared resources", s_multithreadedCreateSharedResourcesGroup));

	return objectMgmtTests.release();
}

} // api
} // vkt
