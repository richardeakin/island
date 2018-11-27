#include "pal_api_loader/ApiRegistry.hpp"

#include "le_renderer/le_renderer.h"
#include "le_renderer/private/le_renderer_types.h"

#include "le_backend_vk/le_backend_vk.h"

#include <cstring>
#include <iostream>
#include <iomanip>
#include <assert.h>
#include <vector>

#define EMPLACE_CMD( x ) new ( &self->mCommandStream[ 0 ] + self->mCommandStreamSize )( x )

struct le_command_buffer_encoder_o {
	char                    mCommandStream[ 4096 * 16 ]; // 16 pages of memory
	size_t                  mCommandStreamSize = 0;
	size_t                  mCommandCount      = 0;
	le_allocator_o *        pAllocator         = nullptr; // allocator is owned by backend, externally
	le_pipeline_manager_o * pipelineManager    = nullptr;
	le_staging_allocator_o *stagingAllocator   = nullptr; // borrowed from backend
};

// ----------------------------------------------------------------------

static le_command_buffer_encoder_o *cbe_create( le_allocator_o *allocator, le_pipeline_manager_o *pipelineManager, le_staging_allocator_o *stagingAllocator ) {
	auto self              = new le_command_buffer_encoder_o;
	self->pAllocator       = allocator;
	self->pipelineManager  = pipelineManager;
	self->stagingAllocator = stagingAllocator;
	//	std::cout << "encoder create : " << std::hex << self << std::endl
	//	          << std::flush;
	return self;
};

// ----------------------------------------------------------------------

static void cbe_destroy( le_command_buffer_encoder_o *self ) {
	//	std::cout << "encoder destroy: " << std::hex << self << std::endl
	//	          << std::flush;
	delete ( self );
}

// ----------------------------------------------------------------------

static void cbe_set_line_width( le_command_buffer_encoder_o *self, float lineWidth ) {

	auto cmd        = EMPLACE_CMD( le::CommandSetLineWidth ); // placement new into data array
	cmd->info.width = lineWidth;

	self->mCommandStreamSize += sizeof( le::CommandSetLineWidth );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_draw( le_command_buffer_encoder_o *self,
                      uint32_t                     vertexCount,
                      uint32_t                     instanceCount,
                      uint32_t                     firstVertex,
                      uint32_t                     firstInstance ) {

	auto cmd  = EMPLACE_CMD( le::CommandDraw ); // placement new!
	cmd->info = {vertexCount, instanceCount, firstVertex, firstInstance};

	self->mCommandStreamSize += sizeof( le::CommandDraw );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_draw_indexed( le_command_buffer_encoder_o *self,
                              uint32_t                     indexCount,
                              uint32_t                     instanceCount,
                              uint32_t                     firstIndex,
                              int32_t                      vertexOffset,
                              uint32_t                     firstInstance ) {

	auto cmd  = EMPLACE_CMD( le::CommandDrawIndexed );
	cmd->info = {
	    indexCount,
	    instanceCount,
	    firstIndex,
	    vertexOffset,
	    firstInstance,
	    0 // padding must be set to zero
	};

	self->mCommandStreamSize += sizeof( le::CommandDrawIndexed );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_set_viewport( le_command_buffer_encoder_o *self,
                              uint32_t                     firstViewport,
                              const uint32_t               viewportCount,
                              const le::Viewport *         pViewports ) {

	auto cmd = EMPLACE_CMD( le::CommandSetViewport ); // placement new!

	// We point data to the next available position in the data stream
	// so that we can store the data for viewports inline.
	void * data     = ( cmd + 1 ); // note: this increments a le::CommandSetViewport pointer by one time its object size, then gets the address
	size_t dataSize = sizeof( le::Viewport ) * viewportCount;

	cmd->info = {firstViewport, viewportCount};
	cmd->header.info.size += dataSize; // we must increase the size of this command by its payload size

	memcpy( data, pViewports, dataSize );

	self->mCommandStreamSize += cmd->header.info.size;
	self->mCommandCount++;
};

// ----------------------------------------------------------------------

static void cbe_set_scissor( le_command_buffer_encoder_o *self,
                             uint32_t                     firstScissor,
                             const uint32_t               scissorCount,
                             le::Rect2D const *           pScissors ) {

	auto cmd = EMPLACE_CMD( le::CommandSetScissor ); // placement new!

	// We point to the next available position in the data stream
	// so that we can store the data for scissors inline.
	void * data     = ( cmd + 1 );
	size_t dataSize = sizeof( le::Rect2D ) * scissorCount;

	cmd->info = {firstScissor, scissorCount};
	cmd->header.info.size += dataSize; // we must increase the size of this command by its payload size

	memcpy( data, pScissors, dataSize );

	self->mCommandStreamSize += cmd->header.info.size;
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_bind_vertex_buffers( le_command_buffer_encoder_o *self,
                                     uint32_t                     firstBinding,
                                     uint32_t                     bindingCount,
                                     le_resource_handle_t const * pBuffers,
                                     uint64_t const *             pOffsets ) {

	// NOTE: pBuffers will hold ids for virtual buffers, we must match these
	// in the backend to actual vulkan buffer ids.
	// Buffer must be annotated whether it is transient or not

	auto cmd = EMPLACE_CMD( le::CommandBindVertexBuffers ); // placement new!

	size_t dataBuffersSize = ( sizeof( le_resource_handle_t ) ) * bindingCount;
	size_t dataOffsetsSize = ( sizeof( uint64_t ) ) * bindingCount;

	void *dataBuffers = ( cmd + 1 );
	void *dataOffsets = ( static_cast<char *>( dataBuffers ) + dataBuffersSize ); // start address for offset data

	cmd->info = {firstBinding, bindingCount, static_cast<le_resource_handle_t *>( dataBuffers ), static_cast<uint64_t *>( dataOffsets )};
	cmd->header.info.size += dataBuffersSize + dataOffsetsSize; // we must increase the size of this command by its payload size

	memcpy( dataBuffers, pBuffers, dataBuffersSize );
	memcpy( dataOffsets, pOffsets, dataOffsetsSize );

	self->mCommandStreamSize += cmd->header.info.size;
	self->mCommandCount++;
}

// ----------------------------------------------------------------------
static void cbe_bind_index_buffer( le_command_buffer_encoder_o *self,
                                   le_resource_handle_t const   buffer,
                                   uint64_t                     offset,
                                   le::IndexType const &        indexType ) {

	auto cmd = EMPLACE_CMD( le::CommandBindIndexBuffer );

	// Note: indexType==0 means uint16, indexType==1 means uint32
	cmd->info = {buffer, offset, static_cast<uint32_t>( indexType )};

	self->mCommandStreamSize += cmd->header.info.size;
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_set_vertex_data( le_command_buffer_encoder_o *self,
                                 void const *                 data,
                                 uint64_t                     numBytes,
                                 uint32_t                     bindingIndex ) {

	// -- Allocate data on scratch buffer
	// -- Upload data via scratch allocator
	// -- Bind vertex buffers to scratch allocator

	using namespace le_backend_vk; // for le_allocator_linear_i

	void *   memAddr;
	uint64_t bufferOffset = 0;

	if ( le_allocator_linear_i.allocate( self->pAllocator, numBytes, &memAddr, &bufferOffset ) ) {
		memcpy( memAddr, data, numBytes );

		le_resource_handle_t allocatorBufferId = le_allocator_linear_i.get_le_resource_id( self->pAllocator );

		cbe_bind_vertex_buffers( self, bindingIndex, 1, &allocatorBufferId, &bufferOffset );
	} else {
		std::cerr << "ERROR " << __PRETTY_FUNCTION__ << " could not allocate " << numBytes << " Bytes." << std::endl
		          << std::flush;
	}
}

// ----------------------------------------------------------------------

static void cbe_set_index_data( le_command_buffer_encoder_o *self,
                                void const *                 data,
                                uint64_t                     numBytes,
                                le::IndexType const &        indexType ) {

	using namespace le_backend_vk; // for le_allocator_linear_i

	void *   memAddr;
	uint64_t bufferOffset = 0;

	// -- Allocate data on scratch buffer
	if ( le_allocator_linear_i.allocate( self->pAllocator, numBytes, &memAddr, &bufferOffset ) ) {

		// -- Upload data via scratch allocator
		memcpy( memAddr, data, numBytes );

		le_resource_handle_t allocatorBufferId = le_allocator_linear_i.get_le_resource_id( self->pAllocator );

		// -- Bind index buffer to scratch allocator
		cbe_bind_index_buffer( self, allocatorBufferId, bufferOffset, indexType );
	} else {
		std::cerr << "ERROR " << __PRETTY_FUNCTION__ << " could not allocate " << numBytes << " Bytes." << std::endl
		          << std::flush;
	}
}

// ----------------------------------------------------------------------

static void cbe_set_argument_ubo_data( le_command_buffer_encoder_o *self,
                                       uint64_t                     argumentNameId, // hash id of argument name
                                       void const *                 data,
                                       size_t                       numBytes ) {

	using namespace le_backend_vk; // for le_allocator_linear_i

	auto cmd = EMPLACE_CMD( le::CommandSetArgumentUbo );

	void *   memAddr;
	uint64_t bufferOffset = 0;

	// -- Allocate memory on scratch buffer for ubo
	//
	// Note that we might want to have specialised ubo memory eventually if that
	// made a performance difference.
	if ( le_allocator_linear_i.allocate( self->pAllocator, numBytes, &memAddr, &bufferOffset ) ) {

		// -- Store ubo data to scratch allocator
		memcpy( memAddr, data, numBytes );

		le_resource_handle_t allocatorBufferId = le_allocator_linear_i.get_le_resource_id( self->pAllocator );

		cmd->info.argument_name_id = argumentNameId;
		cmd->info.buffer_id        = allocatorBufferId;
		cmd->info.offset           = uint32_t( bufferOffset ); // Note: we are assuming offset is never > 4GB, which appears realistic for now
		cmd->info.range            = uint32_t( numBytes );

	} else {
		std::cerr << "ERROR " << __PRETTY_FUNCTION__ << " could not allocate " << numBytes << " Bytes." << std::endl
		          << std::flush;
		return;
	}

	self->mCommandStreamSize += sizeof( le::CommandSetArgumentUbo );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_set_argument_texture( le_command_buffer_encoder_o *self, le_resource_handle_t const textureId, uint64_t argumentName, uint64_t arrayIndex ) {

	auto cmd = EMPLACE_CMD( le::CommandSetArgumentTexture );

	cmd->info.argument_name_id = argumentName;
	cmd->info.texture_id       = textureId;
	cmd->info.array_index      = arrayIndex;

	self->mCommandStreamSize += sizeof( le::CommandSetArgumentTexture );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_bind_pipeline( le_command_buffer_encoder_o *self, uint64_t psoHash ) {

	// -- insert PSO pointer into command stream
	auto cmd = EMPLACE_CMD( le::CommandBindPipeline );

	cmd->info.psoHash = psoHash;

	//	std::cout << "binding pipeline" << std::endl
	//	          << std::flush;

	self->mCommandStreamSize += sizeof( le::CommandBindPipeline );
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_write_to_buffer( le_command_buffer_encoder_o *self, le_resource_handle_t const &resourceId, size_t offset, void const *data, size_t numBytes ) {

	auto cmd = EMPLACE_CMD( le::CommandWriteToBuffer );

	using namespace le_backend_vk; // for le_allocator_linear_i
	void *               memAddr;
	le_resource_handle_t srcResourceId;

	// -- Allocate memory using staging allocator
	//
	// We don't use the encoder local scratch linear allocator, since memory written to buffers is
	// typically a lot larger than uniforms and other small settings structs. Staging memory is also
	// allocated so that it is only used for TRANSFER_SRC, and shared amongst encoders so that we
	// use available memory more efficiently.
	//
	if ( le_staging_allocator_i.map( self->stagingAllocator, numBytes, &memAddr, &srcResourceId ) ) {
		// -- Write data to scratch memory now
		memcpy( memAddr, data, numBytes );

		cmd->info.src_buffer_id = srcResourceId;
		cmd->info.src_offset    = 0; // staging allocator will give us a fresh buffer, and src memory will be placed at its start
		cmd->info.dst_offset    = offset;
		cmd->info.numBytes      = numBytes;
		cmd->info.dst_buffer_id = resourceId;
	} else {
		std::cerr << "ERROR " << __PRETTY_FUNCTION__ << " could not allocate " << numBytes << " Bytes." << std::endl
		          << std::flush;
		return;
	}

	self->mCommandStreamSize += sizeof( le::CommandWriteToBuffer );
	self->mCommandCount++;
}

// Generate mipmap from input data
// adapted from https://github.com/ValveSoftware/openvr/blob/1fb1030f2ac238456dca7615a4408fb2bb42afb6/samples/hellovr_vulkan/hellovr_vulkan_main.cpp#L2271
template <typename PixelType, const size_t numChannels>
static void generate_mipmap( const PixelType *pSrc, PixelType *pDst, uint32_t const nSrcWidth, uint32_t const nSrcHeight, uint32_t *pDstWidthOut, uint32_t *pDstHeightOut ) {

	*pDstWidthOut = nSrcWidth / 2;
	if ( *pDstWidthOut <= 0 ) {
		*pDstWidthOut = 1;
	}
	*pDstHeightOut = nSrcHeight / 2;
	if ( *pDstHeightOut <= 0 ) {
		*pDstHeightOut = 1;
	}

	for ( uint32_t y = 0; y != *pDstHeightOut; y++ ) {
		for ( uint32_t x = 0; x != *pDstWidthOut; x++ ) {

			// We use floats to accumulate pixel values.
			//
			// TODO: if pixels arrive in non-linear SRGB format, we must convert them to linear when
			// we read them, and convert them back to non-linear when we write them back after averaging.
			//
			float channel[ numChannels ]{};

			uint32_t nSrcIndex[ 4 ]; // we reduce 4 neighbouring pixels to 1

			// Get pixel indices
			nSrcIndex[ 0 ] = ( ( ( y * 2 ) * nSrcWidth ) + ( x * 2 ) ) * 4;
			nSrcIndex[ 1 ] = ( ( ( y * 2 ) * nSrcWidth ) + ( x * 2 + 1 ) ) * 4;
			nSrcIndex[ 2 ] = ( ( ( ( y * 2 ) + 1 ) * nSrcWidth ) + ( x * 2 ) ) * 4;
			nSrcIndex[ 3 ] = ( ( ( ( y * 2 ) + 1 ) * nSrcWidth ) + ( x * 2 + 1 ) ) * 4;

			// Sum all pixels
			for ( uint32_t nSample = 0; nSample != 4; nSample++ ) {
				for ( uint32_t c = 0; c != numChannels; c++ ) {
					channel[ c ] += pSrc[ nSrcIndex[ nSample ] + c ];
				}
			}

			// Average results
			for ( uint32_t c = 0; c != numChannels; c++ ) {
				channel[ c ] /= 4.f;
			}

			// Store resulting pixels
			for ( uint32_t c = 0; c != numChannels; c++ ) {
				pDst[ ( y * ( *pDstWidthOut ) + x ) * numChannels + c ] = static_cast<PixelType>( channel[ c ] );
			}
		}
	}
}

// ----------------------------------------------------------------------
// Returns the number of bytes needed to store numMipLevels for an image
// with dimensions at width/height
static size_t getNumBytesRequiredForMipchain( le_resource_info_t::Image const &imageInfo ) {

	size_t numBytesPerTexel = 0;

	switch ( imageInfo.format ) {
	case ( le::Format::eR8G8B8A8Unorm ):   // fall-through
	case ( le::Format::eR8G8B8A8Snorm ):   // fall-through
	case ( le::Format::eR8G8B8A8Uscaled ): // fall-through
	case ( le::Format::eR8G8B8A8Sscaled ): // fall-through
	case ( le::Format::eR8G8B8A8Uint ):    // fall-through
	case ( le::Format::eR8G8B8A8Sint ):    // fall-through
	case ( le::Format::eR8G8B8A8Srgb ):    // fall-through
	case ( le::Format::eB8G8R8A8Unorm ):   // fall-through
	case ( le::Format::eB8G8R8A8Snorm ):   // fall-through
	case ( le::Format::eB8G8R8A8Uscaled ): // fall-through
	case ( le::Format::eB8G8R8A8Sscaled ): // fall-through
	case ( le::Format::eB8G8R8A8Uint ):    // fall-through
	case ( le::Format::eB8G8R8A8Sint ):    // fall-through
	case ( le::Format::eB8G8R8A8Srgb ):    // fall-through
		numBytesPerTexel = 4 * sizeof( uint8_t );
	    break;
	case ( le::Format::eR16G16B16A16Unorm ):   // fall-through
	case ( le::Format::eR16G16B16A16Snorm ):   // fall-through
	case ( le::Format::eR16G16B16A16Uscaled ): // fall-through
	case ( le::Format::eR16G16B16A16Sscaled ): // fall-through
	case ( le::Format::eR16G16B16A16Uint ):    // fall-through
	case ( le::Format::eR16G16B16A16Sint ):    // fall-through
	case ( le::Format::eR16G16B16A16Sfloat ):  // fall-through
		numBytesPerTexel = 4 * sizeof( uint16_t );
	    break;
	default:
		assert( false ); // unhandled format
	}

	assert( numBytesPerTexel != 0 );

	// --------| invariant: number of bytes per texel is valid

	// we do a rough calculation which just double the size of the original image
	size_t totalBytes = 0;

	uint32_t width  = imageInfo.extent.width;
	uint32_t height = imageInfo.extent.height;

	for ( size_t i = 0; i != imageInfo.mipLevels; i++ ) {
		totalBytes += numBytesPerTexel * width * height;
		width  = width > 2 ? width >> 1 : 1;
		height = height > 2 ? height >> 1 : 1;
	}

	return totalBytes;
}

// ----------------------------------------------------------------------
// Writes buffer contents to staging memory (which is allocated on-demand)
// and adds a write-to-image command into the command stream.
//
// TODO: Implement uploading mipmaps based on this example:
//       <https://github.com/ValveSoftware/openvr/blob/1fb1030f2ac238456dca7615a4408fb2bb42afb6/samples/hellovr_vulkan/hellovr_vulkan_main.cpp#L2169>
// we could generate mipmaps here - but how would the image know it was mipmapped? - and how many mipmap levels there are?
// if we had the matching resourceinfo we would have all the information we needed. perhaps we should require it.
static void cbe_write_to_image( le_command_buffer_encoder_o *self,
                                le_resource_handle_t const & resourceId,
                                le_resource_info_t const &   resourceInfo,
                                void const *                 data,
                                size_t                       numBytes ) {

	assert( resourceInfo.type == LeResourceType::eImage );
	auto const &imageInfo = resourceInfo.image;

	auto cmd = EMPLACE_CMD( le::CommandWriteToImage );

	using namespace le_backend_vk; // for le_allocator_linear_i
	void *               memAddr;
	le_resource_handle_t srcResourceId;

	// Check if resourceInfo requests more than one mip level.
	// If so, we must generate the number of mip levels requested.

	// We must also re-calculate the number of bytes based on the number of mip-levels,
	// the image size, and the image format. For now, we only cover a select number of formats.

	// TODO: make this dependent on format, change parameter to use imageInfo directly.
	size_t numBytesForRequestedMipchain = getNumBytesRequiredForMipchain( imageInfo );

	// -- Allocate memory using staging allocator
	//
	// We don't use the encoder local scratch linear allocator, since memory written to buffers is
	// typically a lot larger than uniforms and other small settings structs. Staging memory is also
	// allocated so that it is only used for TRANSFER_SRC, and shared amongst encoders so that we
	// use available memory more efficiently.
	//
	if ( le_staging_allocator_i.map( self->stagingAllocator, numBytesForRequestedMipchain, &memAddr, &srcResourceId ) ) {

		// -- Write data to scratch memory now
		memcpy( memAddr, data, numBytes );

		// Add number of regions to this command matching the number of mip levels
		// so that we can upload multiple mip levels at once.

		auto const regions_begin = reinterpret_cast<le::CommandWriteToImage::ImageWriteRegion *>( cmd + 1 );
		auto const regions_end   = regions_begin + imageInfo.mipLevels;

		auto region = regions_begin;

		region->dstMipLevel        = 0;
		region->dstMipLevelExtentW = imageInfo.extent.width;
		region->dstMipLevelExtentH = imageInfo.extent.height;
		region->srcBufferOffset    = 0;

		// We increase the region pointer, because now, we add regions for mipmaps.
		region++;
		uint64_t dstRegionOffsetInBytes = numBytes; // careful: these offsets need to be in PixelType
		uint64_t srcRegionOffsetInBytes = 0;        // careful: these offsets need to be in PixelType
		uint32_t srcWidth               = imageInfo.extent.width;
		uint32_t srcHeight              = imageInfo.extent.height;

		for ( uint32_t mipLevel = 1; region != regions_end; mipLevel++ ) {
			region->dstMipLevel = mipLevel;

			uint32_t dstWidth  = 0;
			uint32_t dstHeight = 0;

			std::cout << "Generating mipmap level..." << mipLevel << std::flush;

			generate_mipmap<uint8_t, 4>( static_cast<uint8_t *>( memAddr ) + srcRegionOffsetInBytes,
			                             static_cast<uint8_t *>( memAddr ) + dstRegionOffsetInBytes,
			                             srcWidth, srcHeight, &dstWidth, &dstHeight );

			std::cout << " done." << std::endl
			          << std::flush;

			region->dstMipLevelExtentW = dstWidth;
			region->dstMipLevelExtentH = dstHeight;
			region->srcBufferOffset    = uint32_t( dstRegionOffsetInBytes );

			// Store dst -> src  for nex iteration
			//
			srcWidth               = dstWidth;
			srcHeight              = dstHeight;
			srcRegionOffsetInBytes = dstRegionOffsetInBytes;

			// Move dstRegionOffsetInBytes to end of current image

			dstRegionOffsetInBytes += dstWidth * dstHeight * sizeof( uint8_t ) * 4; // assuming 4 channels of uint8_t

			// Next iteration shall work on next region
			region++;
		}

		cmd->info.src_buffer_id = srcResourceId;
		cmd->info.numBytes      = numBytesForRequestedMipchain; // total number of bytes from buffer which must be synchronised
		cmd->info.dst_image_id  = resourceId;
		cmd->info.pRegions      = regions_begin;
		cmd->info.numRegions    = imageInfo.mipLevels;

		cmd->header.info.size += sizeof( le::CommandWriteToImage::ImageWriteRegion ) * cmd->info.numRegions;
	} else {
		std::cerr << "ERROR " << __PRETTY_FUNCTION__ << " could not allocate " << numBytesForRequestedMipchain << " Bytes." << std::endl
		          << std::flush;
		return;
	}
	// increase command stream size by size of command, plus size of regions attached to command.
	self->mCommandStreamSize += cmd->header.info.size;
	self->mCommandCount++;
}

// ----------------------------------------------------------------------

static void cbe_get_encoded_data( le_command_buffer_encoder_o *self,
                                  void **                      data,
                                  size_t *                     numBytes,
                                  size_t *                     numCommands ) {

	*data        = self->mCommandStream;
	*numBytes    = self->mCommandStreamSize;
	*numCommands = self->mCommandCount;
}

// ----------------------------------------------------------------------

static le_pipeline_manager_o *cbe_get_pipeline_manager( le_command_buffer_encoder_o *self ) {
	return self->pipelineManager;
}

// ----------------------------------------------------------------------

ISL_API_ATTR void register_le_command_buffer_encoder_api( void *api_ ) {

	auto &cbe_i = static_cast<le_renderer_api *>( api_ )->le_command_buffer_encoder_i;

	cbe_i.create                 = cbe_create;
	cbe_i.destroy                = cbe_destroy;
	cbe_i.draw                   = cbe_draw;
	cbe_i.draw_indexed           = cbe_draw_indexed;
	cbe_i.set_line_width         = cbe_set_line_width;
	cbe_i.set_viewport           = cbe_set_viewport;
	cbe_i.set_scissor            = cbe_set_scissor;
	cbe_i.bind_vertex_buffers    = cbe_bind_vertex_buffers;
	cbe_i.bind_index_buffer      = cbe_bind_index_buffer;
	cbe_i.set_index_data         = cbe_set_index_data;
	cbe_i.set_vertex_data        = cbe_set_vertex_data;
	cbe_i.set_argument_ubo_data  = cbe_set_argument_ubo_data;
	cbe_i.set_argument_texture   = cbe_set_argument_texture;
	cbe_i.bind_graphics_pipeline = cbe_bind_pipeline;
	cbe_i.get_encoded_data       = cbe_get_encoded_data;
	cbe_i.write_to_buffer        = cbe_write_to_buffer;
	cbe_i.write_to_image         = cbe_write_to_image;
	cbe_i.get_pipeline_manager   = cbe_get_pipeline_manager;
}
