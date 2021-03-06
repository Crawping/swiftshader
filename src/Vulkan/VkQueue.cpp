// Copyright 2018 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "VkCommandBuffer.hpp"
#include "VkFence.hpp"
#include "VkQueue.hpp"
#include "VkSemaphore.hpp"
#include "WSI/VkSwapchainKHR.hpp"

#include <cstring>

namespace
{

VkSubmitInfo* DeepCopySubmitInfo(uint32_t submitCount, const VkSubmitInfo* pSubmits)
{
	size_t submitSize = sizeof(VkSubmitInfo) * submitCount;
	size_t totalSize = submitSize;
	for(uint32_t i = 0; i < submitCount; i++)
	{
		totalSize += pSubmits[i].waitSemaphoreCount * sizeof(VkSemaphore);
		totalSize += pSubmits[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags);
		totalSize += pSubmits[i].signalSemaphoreCount * sizeof(VkSemaphore);
		totalSize += pSubmits[i].commandBufferCount * sizeof(VkCommandBuffer);
	}

	uint8_t* mem = static_cast<uint8_t*>(
		vk::allocate(totalSize, vk::REQUIRED_MEMORY_ALIGNMENT, vk::DEVICE_MEMORY, vk::Fence::GetAllocationScope()));

	auto submits = new (mem) VkSubmitInfo[submitCount];
	memcpy(mem, pSubmits, submitSize);
	mem += submitSize;

	for(uint32_t i = 0; i < submitCount; i++)
	{
		size_t size = pSubmits[i].waitSemaphoreCount * sizeof(VkSemaphore);
		submits[i].pWaitSemaphores = new (mem) VkSemaphore[pSubmits[i].waitSemaphoreCount];
		memcpy(mem, pSubmits[i].pWaitSemaphores, size);
		mem += size;

		size = pSubmits[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags);
		submits[i].pWaitDstStageMask = new (mem) VkPipelineStageFlags[pSubmits[i].waitSemaphoreCount];
		memcpy(mem, pSubmits[i].pWaitDstStageMask, size);
		mem += size;

		size = pSubmits[i].signalSemaphoreCount * sizeof(VkSemaphore);
		submits[i].pSignalSemaphores = new (mem) VkSemaphore[pSubmits[i].signalSemaphoreCount];
		memcpy(mem, pSubmits[i].pSignalSemaphores, size);
		mem += size;

		size = pSubmits[i].commandBufferCount * sizeof(VkCommandBuffer);
		submits[i].pCommandBuffers = new (mem) VkCommandBuffer[pSubmits[i].commandBufferCount];
		memcpy(mem, pSubmits[i].pCommandBuffers, size);
		mem += size;
	}

	return submits;
}

} // anonymous namespace

namespace vk
{

Queue::Queue()
{
	context.reset(new sw::Context());
	renderer.reset(new sw::Renderer(context.get(), sw::OpenGL, true));

	queueThread = std::thread(TaskLoop, this);
}

void Queue::destroy()
{
	Task task;
	task.type = Task::KILL_THREAD;
	pending.put(task);

	queueThread.join();
	ASSERT_MSG(pending.count() == 0, "queue has work after worker thread shutdown");

	garbageCollect();

	renderer.reset(nullptr);
	context.reset(nullptr);
}

VkResult Queue::submit(uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
	garbageCollect();

	Task task;
	task.submitCount = submitCount;
	task.pSubmits = DeepCopySubmitInfo(submitCount, pSubmits);
	task.fence = (fence != VK_NULL_HANDLE) ? vk::Cast(fence) : nullptr;

	if(task.fence)
	{
		task.fence->add();
	}

	pending.put(task);

	return VK_SUCCESS;
}

void Queue::TaskLoop(vk::Queue* queue)
{
	queue->taskLoop();
}

void Queue::submitQueue(const Task& task)
{
	for(uint32_t i = 0; i < task.submitCount; i++)
	{
		auto& submitInfo = task.pSubmits[i];
		for(uint32_t j = 0; j < submitInfo.waitSemaphoreCount; j++)
		{
			vk::Cast(submitInfo.pWaitSemaphores[j])->wait(submitInfo.pWaitDstStageMask[j]);
		}

		{
			CommandBuffer::ExecutionState executionState;
			executionState.renderer = renderer.get();
			executionState.fence = task.fence;
			for(uint32_t j = 0; j < submitInfo.commandBufferCount; j++)
			{
				vk::Cast(submitInfo.pCommandBuffers[j])->submit(executionState);
			}
		}

		for(uint32_t j = 0; j < submitInfo.signalSemaphoreCount; j++)
		{
			vk::Cast(submitInfo.pSignalSemaphores[j])->signal();
		}
	}

	if (task.pSubmits)
	{
		toDelete.put(task.pSubmits);
	}

	if(task.fence)
	{
		task.fence->done();
	}
}

void Queue::taskLoop()
{
	while(true)
	{
		Task task = pending.take();

		switch(task.type)
		{
		case Task::KILL_THREAD:
			ASSERT_MSG(pending.count() == 0, "queue has remaining work!");
			return;
		case Task::SUBMIT_QUEUE:
			submitQueue(task);
			break;
		default:
			UNIMPLEMENTED("task.type %d", static_cast<int>(task.type));
			break;
		}
	}
}

VkResult Queue::waitIdle()
{
	// Wait for task queue to flush.
	vk::Fence fence;
	fence.add();

	Task task;
	task.fence = &fence;
	pending.put(task);

	fence.wait();

	// Wait for all draw operations to complete, if any
	renderer->synchronize();

	garbageCollect();

	return VK_SUCCESS;
}

void Queue::garbageCollect()
{
	while (true)
	{
		auto v = toDelete.tryTake();
		if (!v.second) { break; }
		vk::deallocate(v.first, DEVICE_MEMORY);
	}
}

#ifndef __ANDROID__
void Queue::present(const VkPresentInfoKHR* presentInfo)
{
	for(uint32_t i = 0; i < presentInfo->waitSemaphoreCount; i++)
	{
		vk::Cast(presentInfo->pWaitSemaphores[i])->wait();
	}

	for(uint32_t i = 0; i < presentInfo->swapchainCount; i++)
	{
		vk::Cast(presentInfo->pSwapchains[i])->present(presentInfo->pImageIndices[i]);
	}
}
#endif

} // namespace vk
