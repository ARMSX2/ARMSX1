#pragma once
#include "vulkan_gpu.h"
#include <jni.h>

PsxVulkanContext psxVulkanGpuContext(jlong output);
bool psxVulkanPresentImage(JNIEnv* env,jlong output,jobject surface,VkImage image,int width,int height,bool filter,jlong token);
void psxVulkanForgetImage(jlong output,VkImage image);
void psxVulkanNativeDimensions(jlong output,int width,int height);
