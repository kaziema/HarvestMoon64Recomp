// Null Plume backend for the Vita (Phase 0 step 2). See plume_null.h.

#include "plume_null.h"

#include <atomic>
#include <cstdlib>
#include <memory>

#include "plume_render_interface.h"

#include "vita_log.h"

namespace {
    std::atomic<uint32_t> stat_command_lists{0};
    std::atomic<uint32_t> stat_draws{0};
    std::atomic<uint32_t> stat_dispatches{0};
    std::atomic<uint32_t> stat_copies{0};
    std::atomic<uint32_t> stat_presents{0};
    std::atomic<uint32_t> stat_buffers{0};
    std::atomic<uint32_t> stat_textures{0};
    std::atomic<uint64_t> stat_mapped_bytes{0};

    // The Vita's screen. RT64 sizes its output from the swap chain.
    constexpr uint32_t ScreenWidth = 960;
    constexpr uint32_t ScreenHeight = 544;
}

namespace plume {
    struct NullBufferFormattedView : RenderBufferFormattedView {};

    struct NullBuffer : RenderBuffer {
        RenderBufferDesc desc;
        void *memory = nullptr;

        NullBuffer(const RenderBufferDesc &desc) : desc(desc) {
            stat_buffers++;
        }

        ~NullBuffer() override {
            if (memory != nullptr) {
                stat_mapped_bytes -= desc.size;
                free(memory);
            }
            stat_buffers--;
        }

        // RT64 writes uploads into mapped buffers (and reads readback buffers), so they need real memory.
        // It is allocated the first time the buffer is mapped; buffers that are never mapped cost nothing.
        void *map(uint32_t subresource, const RenderRange *readRange) override {
            if (memory == nullptr) {
                memory = calloc(1, size_t(desc.size > 0 ? desc.size : 1));
                if (memory == nullptr) {
                    hm64vita::log_line("plume_null: out of memory mapping a %llu byte buffer", (unsigned long long)desc.size);
                    return nullptr;
                }
                stat_mapped_bytes += desc.size;
            }
            return memory;
        }

        void unmap(uint32_t subresource, const RenderRange *writtenRange) override {}

        std::unique_ptr<RenderBufferFormattedView> createBufferFormattedView(RenderFormat format) override {
            return std::make_unique<NullBufferFormattedView>();
        }

        void setName(const std::string &name) override {}

        uint64_t getDeviceAddress() const override {
            return 0;
        }
    };

    struct NullTextureView : RenderTextureView {};

    struct NullTexture : RenderTexture {
        RenderTextureDesc desc;

        NullTexture(const RenderTextureDesc &desc) : desc(desc) {
            stat_textures++;
        }

        ~NullTexture() override {
            stat_textures--;
        }

        std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc &desc) const override {
            return std::make_unique<NullTextureView>();
        }

        void setName(const std::string &name) override {}
    };

    struct NullAccelerationStructure : RenderAccelerationStructure {};

    struct NullShader : RenderShader {
        void setName(const std::string &name) override {}
    };

    struct NullSampler : RenderSampler {};

    struct NullPipeline : RenderPipeline {
        void setName(const std::string &name) override {}

        RenderPipelineProgram getProgram(const std::string &name) const override {
            return RenderPipelineProgram();
        }
    };

    struct NullPipelineLayout : RenderPipelineLayout {};
    struct NullCommandFence : RenderCommandFence {};
    struct NullCommandSemaphore : RenderCommandSemaphore {};

    struct NullDescriptorSet : RenderDescriptorSet {
        void setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *bufferStructuredView, const RenderBufferFormattedView *bufferFormattedView) override {}
        void setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout textureLayout, const RenderTextureView *textureView) override {}
        void setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) override {}
        void setAccelerationStructure(uint32_t descriptorIndex, const RenderAccelerationStructure *accelerationStructure) override {}
    };

    struct NullFramebuffer : RenderFramebuffer {
        uint32_t width = 0;
        uint32_t height = 0;

        NullFramebuffer(const RenderFramebufferDesc &desc) {
            const RenderTexture *texture = nullptr;
            if ((desc.colorAttachmentsCount > 0) && (desc.colorAttachments != nullptr)) {
                texture = desc.colorAttachments[0];
            }
            else {
                texture = desc.depthAttachment;
            }

            if (texture != nullptr) {
                const NullTexture *nullTexture = static_cast<const NullTexture *>(texture);
                width = nullTexture->desc.width;
                height = nullTexture->desc.height;
            }
        }

        uint32_t getWidth() const override {
            return width;
        }

        uint32_t getHeight() const override {
            return height;
        }
    };

    struct NullSwapChain : RenderSwapChain {
        RenderSwapChainDesc desc;
        std::vector<std::unique_ptr<NullTexture>> textures;
        uint32_t nextTexture = 0;
        bool vsync = true;

        NullSwapChain(const RenderSwapChainDesc &desc) : desc(desc) {
            const uint32_t count = desc.textureCount > 0 ? desc.textureCount : 2;
            for (uint32_t i = 0; i < count; i++) {
                RenderTextureDesc textureDesc = RenderTextureDesc::Texture2D(ScreenWidth, ScreenHeight, 1, desc.format, RenderTextureFlag::RENDER_TARGET);
                textures.emplace_back(std::make_unique<NullTexture>(textureDesc));
            }
        }

        bool present(uint32_t textureIndex, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount) override {
            stat_presents++;
            return true;
        }

        void wait() override {}

        bool resize() override {
            return true;
        }

        bool needsResize() const override {
            return false;
        }

        void setVsyncEnabled(bool vsyncEnabled) override {
            vsync = vsyncEnabled;
        }

        bool isVsyncEnabled() const override {
            return vsync;
        }

        uint32_t getWidth() const override {
            return ScreenWidth;
        }

        uint32_t getHeight() const override {
            return ScreenHeight;
        }

        RenderTexture *getTexture(uint32_t textureIndex) override {
            return textures[textureIndex].get();
        }

        uint32_t getTextureCount() const override {
            return uint32_t(textures.size());
        }

        bool acquireTexture(RenderCommandSemaphore *signalSemaphore, uint32_t *textureIndex) override {
            *textureIndex = nextTexture;
            nextTexture = (nextTexture + 1) % uint32_t(textures.size());
            return true;
        }

        RenderWindow getWindow() const override {
            return desc.renderWindow;
        }

        bool isEmpty() const override {
            return false;
        }

        uint32_t getRefreshRate() const override {
            return 60;
        }
    };

    struct NullCommandList : RenderCommandList {
        void begin() override {}
        void end() override {}
        void barriers(RenderBarrierStages stages, const RenderBufferBarrier *bufferBarriers, uint32_t bufferBarriersCount, const RenderTextureBarrier *textureBarriers, uint32_t textureBarriersCount) override {}

        void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override {
            stat_dispatches++;
        }

        void traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override {}

        void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override {
            stat_draws++;
        }

        void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override {
            stat_draws++;
        }

        void setPipeline(const RenderPipeline *pipeline) override {}
        void setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) override {}
        void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override {}
        void setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override {}
        void setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) override {}
        void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override {}
        void setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override {}
        void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override {}
        void setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) override {}
        void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override {}
        void setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override {}
        void setIndexBuffer(const RenderIndexBufferView *view) override {}
        void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) override {}
        void setViewports(const RenderViewport *viewports, uint32_t count) override {}
        void setScissors(const RenderRect *scissorRects, uint32_t count) override {}
        void setFramebuffer(const RenderFramebuffer *framebuffer) override {}
        void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override {}
        void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) override {}
        void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) override {}

        void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override {
            stat_copies++;
        }

        void copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) override {
            stat_copies++;
        }

        void copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) override {
            stat_copies++;
        }

        void copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override {
            stat_copies++;
        }

        void resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override {}
        void resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) override {}
        void buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) override {}
        void buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) override {}
        void discardTexture(const RenderTexture *texture) override {}
        void resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) override {}
        void writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) override {}
    };

    struct NullCommandQueue : RenderCommandQueue {
        std::unique_ptr<RenderCommandList> createCommandList() override {
            return std::make_unique<NullCommandList>();
        }

        std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc &desc) override {
            return std::make_unique<NullSwapChain>(desc);
        }

        // Nothing runs on a GPU, so submitted work is finished as soon as it is submitted.
        void executeCommandLists(const RenderCommandList **commandLists, uint32_t commandListCount, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount, RenderCommandSemaphore **signalSemaphores, uint32_t signalSemaphoreCount, RenderCommandFence *signalFence) override {
            stat_command_lists += commandListCount;
        }

        void waitForCommandFence(RenderCommandFence *fence) override {}
    };

    struct NullPool : RenderPool {
        std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override {
            return std::make_unique<NullBuffer>(desc);
        }

        std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override {
            return std::make_unique<NullTexture>(desc);
        }
    };

    struct NullQueryPool : RenderQueryPool {
        std::vector<uint64_t> results;

        NullQueryPool(uint32_t count) : results(count, 0) {}

        void queryResults() override {}

        const uint64_t *getResults() const override {
            return results.data();
        }

        uint32_t getCount() const override {
            return uint32_t(results.size());
        }
    };

    struct NullDevice : RenderDevice {
        RenderDeviceCapabilities capabilities;
        RenderDeviceDescription description;

        NullDevice() {
            // Steers RT64 onto its plain path: no ray tracing, no MSAA sample positions, no HDR, no present wait.
            capabilities.maxTextureSize = 4096;
            capabilities.queryPools = false;
            description.name = "Vita null backend";
            description.type = RenderDeviceType::UNKNOWN;
            description.vendor = RenderDeviceVendor::UNKNOWN;
            description.dedicatedVideoMemory = 128ull * 1024 * 1024;
        }

        std::unique_ptr<RenderDescriptorSet> createDescriptorSet(const RenderDescriptorSetDesc &desc) override {
            return std::make_unique<NullDescriptorSet>();
        }

        std::unique_ptr<RenderShader> createShader(const void *data, uint64_t size, const char *entryPointName, RenderShaderFormat format) override {
            return std::make_unique<NullShader>();
        }

        std::unique_ptr<RenderSampler> createSampler(const RenderSamplerDesc &desc) override {
            return std::make_unique<NullSampler>();
        }

        std::unique_ptr<RenderPipeline> createComputePipeline(const RenderComputePipelineDesc &desc) override {
            return std::make_unique<NullPipeline>();
        }

        std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) override {
            return std::make_unique<NullPipeline>();
        }

        std::unique_ptr<RenderPipeline> createRaytracingPipeline(const RenderRaytracingPipelineDesc &desc, const RenderPipeline *previousPipeline) override {
            return std::make_unique<NullPipeline>();
        }

        std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType type) override {
            return std::make_unique<NullCommandQueue>();
        }

        std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override {
            return std::make_unique<NullBuffer>(desc);
        }

        std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override {
            return std::make_unique<NullTexture>(desc);
        }

        std::unique_ptr<RenderAccelerationStructure> createAccelerationStructure(const RenderAccelerationStructureDesc &desc) override {
            return std::make_unique<NullAccelerationStructure>();
        }

        std::unique_ptr<RenderPool> createPool(const RenderPoolDesc &desc) override {
            return std::make_unique<NullPool>();
        }

        std::unique_ptr<RenderPipelineLayout> createPipelineLayout(const RenderPipelineLayoutDesc &desc) override {
            return std::make_unique<NullPipelineLayout>();
        }

        std::unique_ptr<RenderCommandFence> createCommandFence() override {
            return std::make_unique<NullCommandFence>();
        }

        std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore() override {
            return std::make_unique<NullCommandSemaphore>();
        }

        std::unique_ptr<RenderFramebuffer> createFramebuffer(const RenderFramebufferDesc &desc) override {
            return std::make_unique<NullFramebuffer>(desc);
        }

        std::unique_ptr<RenderQueryPool> createQueryPool(uint32_t queryCount) override {
            return std::make_unique<NullQueryPool>(queryCount);
        }

        void setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &buildInfo, const RenderBottomLevelASMesh *meshes, uint32_t meshCount, bool preferFastBuild, bool preferFastTrace) override {}
        void setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &buildInfo, const RenderTopLevelASInstance *instances, uint32_t instanceCount, bool preferFastBuild, bool preferFastTrace) override {}
        void setShaderBindingTableInfo(RenderShaderBindingTableInfo &tableInfo, const RenderShaderBindingGroups &groups, const RenderPipeline *pipeline, RenderDescriptorSet **descriptorSets, uint32_t descriptorSetCount) override {}

        const RenderDeviceCapabilities &getCapabilities() const override {
            return capabilities;
        }

        const RenderDeviceDescription &getDescription() const override {
            return description;
        }

        RenderSampleCounts getSampleCountsSupported(RenderFormat format) const override {
            return RenderSampleCount::COUNT_1;
        }

        bool beginCapture() override {
            return false;
        }

        bool endCapture() override {
            return false;
        }
    };

    struct NullInterface : RenderInterface {
        RenderInterfaceCapabilities capabilities;
        std::vector<std::string> deviceNames = { "Vita null backend" };

        NullInterface() {
            // RT64 picks and specializes its SPIR-V shaders on this path; the blobs are real (vita/build_rt64_shaders.sh).
            capabilities.shaderFormat = RenderShaderFormat::SPIRV;
        }

        std::unique_ptr<RenderDevice> createDevice(const std::string &preferredDeviceName) override {
            return std::make_unique<NullDevice>();
        }

        const std::vector<std::string> &getDeviceNames() const override {
            return deviceNames;
        }

        const RenderInterfaceCapabilities &getCapabilities() const override {
            return capabilities;
        }
    };

    // Called by RT64 (hle/rt64_application.cpp, Vita patch) in place of its Vulkan backend.
    std::unique_ptr<RenderInterface> CreateVitaInterface(RenderWindow renderWindow) {
        hm64vita::log_line("plume_null: creating the null render interface");
        return std::make_unique<NullInterface>();
    }
}

hm64vita::PlumeNullStats hm64vita::get_plume_null_stats() {
    PlumeNullStats stats;
    stats.command_lists_executed = stat_command_lists.load();
    stats.draws = stat_draws.load();
    stats.dispatches = stat_dispatches.load();
    stats.copies = stat_copies.load();
    stats.presents = stat_presents.load();
    stats.buffers_alive = stat_buffers.load();
    stats.textures_alive = stat_textures.load();
    stats.mapped_bytes = stat_mapped_bytes.load();
    return stats;
}
