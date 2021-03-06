//
//  GL41Backend.h
//  libraries/gpu/src/gpu
//
//  Created by Sam Gateau on 10/27/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#ifndef hifi_gpu_41_GL41Backend_h
#define hifi_gpu_41_GL41Backend_h

#include <gl/Config.h>

#include "../gl/GLBackend.h"
#include "../gl/GLTexture.h"

#define GPU_CORE_41 410
#define GPU_CORE_43 430

#ifdef Q_OS_MAC
#define GPU_INPUT_PROFILE GPU_CORE_41
#else 
#define GPU_INPUT_PROFILE GPU_CORE_43
#endif

namespace gpu { namespace gl41 {

using namespace gpu::gl;

class GL41Backend : public GLBackend {
    using Parent = GLBackend;
    // Context Backend static interface required
    friend class Context;

public:
    explicit GL41Backend(bool syncCache) : Parent(syncCache) {}
    GL41Backend() : Parent() {}

    class GL41Texture : public GLTexture {
        using Parent = GLTexture;
        friend class GL41Backend;
        static GLuint allocate(const Texture& texture);
    protected:
        GL41Texture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
        void generateMips() const override;
        void copyMipFaceLinesFromTexture(uint16_t mip, uint8_t face, const uvec3& size, uint32_t yOffset, GLenum format, GLenum type, const void* sourcePointer) const override;
        virtual void syncSampler() const;

        void withPreservedTexture(std::function<void()> f) const;
    };

    //
    // Textures that have fixed allocation sizes and cannot be managed at runtime
    //

    class GL41FixedAllocationTexture : public GL41Texture {
        using Parent = GL41Texture;
        friend class GL41Backend;

    public:
        GL41FixedAllocationTexture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
        ~GL41FixedAllocationTexture();

    protected:
        Size size() const override { return _size; }
        void allocateStorage() const;
        void syncSampler() const override;
        const Size _size { 0 };
    };

    class GL41AttachmentTexture : public GL41FixedAllocationTexture {
        using Parent = GL41FixedAllocationTexture;
        friend class GL41Backend;
    protected:
        GL41AttachmentTexture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
        ~GL41AttachmentTexture();
    };

    class GL41StrictResourceTexture : public GL41FixedAllocationTexture {
        using Parent = GL41FixedAllocationTexture;
        friend class GL41Backend;
    protected:
        GL41StrictResourceTexture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
    };

    class GL41VariableAllocationTexture : public GL41Texture, public GLVariableAllocationSupport {
        using Parent = GL41Texture;
        friend class GL41Backend;
        using PromoteLambda = std::function<void()>;


    protected:
        GL41VariableAllocationTexture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
        ~GL41VariableAllocationTexture();

        void allocateStorage(uint16 allocatedMip);
        void syncSampler() const override;
        void promote() override;
        void demote() override;
        void populateTransferQueue() override;
        void copyMipFaceLinesFromTexture(uint16_t mip, uint8_t face, const uvec3& size, uint32_t yOffset, GLenum format, GLenum type, const void* sourcePointer) const override;

        Size size() const override { return _size; }
        Size _size { 0 };
    };

    class GL41ResourceTexture : public GL41VariableAllocationTexture {
        using Parent = GL41VariableAllocationTexture;
        friend class GL41Backend;
    protected:
        GL41ResourceTexture(const std::weak_ptr<GLBackend>& backend, const Texture& texture);
        ~GL41ResourceTexture();
    };

protected:
    GLuint getFramebufferID(const FramebufferPointer& framebuffer) override;
    GLFramebuffer* syncGPUObject(const Framebuffer& framebuffer) override;

    GLuint getBufferID(const Buffer& buffer) override;
    GLBuffer* syncGPUObject(const Buffer& buffer) override;

    GLTexture* syncGPUObject(const TexturePointer& texture) override;

    GLuint getQueryID(const QueryPointer& query) override;
    GLQuery* syncGPUObject(const Query& query) override;

    // Draw Stage
    void do_draw(const Batch& batch, size_t paramOffset) override;
    void do_drawIndexed(const Batch& batch, size_t paramOffset) override;
    void do_drawInstanced(const Batch& batch, size_t paramOffset) override;
    void do_drawIndexedInstanced(const Batch& batch, size_t paramOffset) override;
    void do_multiDrawIndirect(const Batch& batch, size_t paramOffset) override;
    void do_multiDrawIndexedIndirect(const Batch& batch, size_t paramOffset) override;

    // Input Stage
    void resetInputStage() override;
    void updateInput() override;

    // Synchronize the state cache of this Backend with the actual real state of the GL Context
    void transferTransformState(const Batch& batch) const override;
    void initTransform() override;
    void updateTransform(const Batch& batch) override;

    // Output stage
    void do_blit(const Batch& batch, size_t paramOffset) override;
};

} }

Q_DECLARE_LOGGING_CATEGORY(gpugl41logging)


#endif
