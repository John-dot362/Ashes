/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#include "Miscellaneous/D3D11DeviceMemory.hpp"

#include "Core/D3D11Device.hpp"
#include "Core/D3D11Instance.hpp"
#include "Core/D3D11PhysicalDevice.hpp"

#include <ashes/common/Exception.hpp>

#include "ashesd3d11_api.hpp"

#include <cstdlib>

#pragma warning( push )
#pragma warning( disable: 5262 )
#include <iostream>
#pragma warning( pop )

namespace ashes::D3D11_NAMESPACE
{
	//*********************************************************************************************

	namespace mem
	{
		static VkMemoryPropertyFlags getMemoryProperties( uint32_t memoryTypeIndex )
		{
			assert( memoryTypeIndex < Instance::getMemoryProperties().memoryTypeCount
				&& "Wrong deduced memory type" );
			return Instance::getMemoryProperties().memoryTypes[memoryTypeIndex].propertyFlags;
		}

		static UINT getBindFlags( VkImageCreateInfo const & createInfo
			, VkMemoryPropertyFlags propertyFlags )
		{
			return ( ( isSampled( createInfo.usage )
					? D3D11_BIND_SHADER_RESOURCE
					: 0u )
				| ( isRenderable( createInfo.usage, createInfo.format, createInfo.mipLevels )
					? ( isHostVisible( propertyFlags )
						? 0u
						: ( isDepthOrStencilFormat( createInfo.format )
							? D3D11_BIND_DEPTH_STENCIL
							: D3D11_BIND_RENDER_TARGET ) )
					: 0u )
				| ( isStorage( createInfo.usage )
					? D3D11_BIND_UNORDERED_ACCESS
					: 0 ) );
		}

		static UINT getMiscFlags( VkImageCreateInfo const & createInfo )
		{
			return 0u
				| ( checkFlag( createInfo.flags, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT )
					? D3D11_RESOURCE_MISC_TEXTURECUBE
					: 0u );
		}
	}

	//*********************************************************************************************

	ObjectMemory::ObjectMemory( VkDevice device
		, VkDeviceMemory deviceMemory
		, ID3D11Resource * resource
		, VkDeviceSize offset
		, VkMemoryAllocateInfo allocateInfo
		, D3D11_MAP mapFlags )
		: device{ device }
		, deviceMemory{ deviceMemory }
		, resource{ resource }
		, offset{ offset }
		, size{ allocateInfo.allocationSize }
		, allocateInfo{ std::move( allocateInfo ) }
		, mapFlags{ mapFlags }
	{
		if ( !resource )
		{
			throw ashes::Exception{ VK_ERROR_OUT_OF_DEVICE_MEMORY, "D3D11 resource creation failed" };
		}
	}

	ObjectMemory::ObjectMemory( ObjectMemory && rhs )noexcept
		: device{ rhs.device }
		, deviceMemory{ rhs.deviceMemory }
		, resource{ rhs.resource }
		, offset{ rhs.offset }
		, size{ rhs.size }
		, allocateInfo{ std::move( rhs.allocateInfo ) }
		, mapFlags{ rhs.mapFlags }
		, subresources{ std::move( rhs.subresources ) }
	{
		rhs.device = nullptr;
		rhs.deviceMemory = nullptr;
		rhs.resource = nullptr;
		rhs.offset = 0;
		rhs.size = 0;
		rhs.mapFlags = D3D11_MAP( 0 );
	}

	ObjectMemory & ObjectMemory::operator=( ObjectMemory && rhs )noexcept
	{
		if ( this != &rhs )
		{
			safeRelease( resource );
		}
		device = rhs.device;
		deviceMemory = rhs.deviceMemory;
		resource = rhs.resource;
		offset = rhs.offset;
		size = rhs.size;
		allocateInfo = std::move( rhs.allocateInfo );
		mapFlags = rhs.mapFlags;
		subresources = std::move( rhs.subresources );

		rhs.device = nullptr;
		rhs.deviceMemory = nullptr;
		rhs.resource = nullptr;
		rhs.offset = 0;
		rhs.size = 0;
		rhs.mapFlags = D3D11_MAP( 0 );

		return *this;
	}

	ObjectMemory::~ObjectMemory()noexcept
	{
		safeRelease( resource );
	}

	VkResult ObjectMemory::lock( ID3D11DeviceContext * context
		, UINT subresource
		, D3D11_MAPPED_SUBRESOURCE & data )const
	{
		if ( !resource )
		{
			return VK_ERROR_MEMORY_MAP_FAILED;
		}

		if ( HRESULT hr = context->Map( resource
				, subresource
				, mapFlags
				, 0
				, &data );
			!checkError( device, hr, "LockMemory" ) )
		{
			return VK_ERROR_MEMORY_MAP_FAILED;
		}

		return VK_SUCCESS;
	}

	void ObjectMemory::unlock( ID3D11DeviceContext * context
		, UINT subresource )const
	{
		context->Unmap( resource, subresource );
	}

	void ObjectMemory::upload( DeviceContextLock const & context
		, uint8_t const * data
		, UINT subresource
		, VkDeviceSize poffset
		, VkDeviceSize psize )const
	{
		if ( !data || !resource || subresource >= subresources.size() || offset >= allocateInfo.allocationSize )
		{
			return;
		}

		auto allocationSize = allocateInfo.allocationSize;
		auto objectSize = std::min( size, allocationSize - offset );
		auto objectEnd = offset + objectSize;
		auto requestOffset = std::min( poffset, allocationSize );
		auto requestSize = ( psize == WholeSize ? allocationSize - requestOffset : std::min( psize, allocationSize - requestOffset ) );
		auto copyStart = std::max( offset, requestOffset );
		auto copyEnd = std::min( objectEnd, requestOffset + requestSize );
		if ( copyEnd <= copyStart )
		{
			return;
		}

		auto objectOffset = copyStart - offset;
		auto copySize = copyEnd - copyStart;
		auto & layout = subresources[subresource];
		D3D11_MAPPED_SUBRESOURCE mapped{};

		if ( lock( *context, subresource, mapped ) == VK_SUCCESS )
		{
			if ( mapped.pData && mapped.DepthPitch > 0u && mapped.RowPitch > 0u )
			{
				auto dst = static_cast< uint8_t * >( mapped.pData ) + objectOffset;
				auto src = data + copyStart;

				if ( mapped.DepthPitch == layout.SysMemSlicePitch || mapped.RowPitch == layout.SysMemPitch )
				{
					copySize = std::min( copySize, VkDeviceSize( mapped.DepthPitch ) - std::min( objectOffset, VkDeviceSize( mapped.DepthPitch ) ) );
					if ( copySize > 0u ) std::memcpy( dst, src, size_t( copySize ) );
				}
				else if ( layout.SysMemPitch > 0u )
				{
					auto mappedSteps = mapped.DepthPitch / mapped.RowPitch;
					auto systemSteps = layout.SysMemSlicePitch / layout.SysMemPitch;
					auto rowSize = std::min( mapped.RowPitch, layout.SysMemPitch );
					for ( UINT step = 0u; step < std::min( mappedSteps, systemSteps ); ++step )
					{
						auto rowOffset = VkDeviceSize( step ) * layout.SysMemPitch;
						if ( rowOffset >= copySize ) break;
						auto bytes = std::min( VkDeviceSize( rowSize ), copySize - rowOffset );
						std::memcpy( static_cast< uint8_t * >( mapped.pData ) + VkDeviceSize( step ) * mapped.RowPitch, src + rowOffset, size_t( bytes ) );
					}
				}
			}
			unlock( *context, subresource );
		}
	}

	void ObjectMemory::download( DeviceContextLock const & context
		, uint8_t * data
		, UINT subresource
		, VkDeviceSize poffset
		, VkDeviceSize psize )const
	{
		if ( !data || !resource || subresource >= subresources.size() || offset >= allocateInfo.allocationSize )
		{
			return;
		}

		auto allocationSize = allocateInfo.allocationSize;
		auto objectSize = std::min( size, allocationSize - offset );
		auto objectEnd = offset + objectSize;
		auto requestOffset = std::min( poffset, allocationSize );
		auto requestSize = ( psize == WholeSize ? allocationSize - requestOffset : std::min( psize, allocationSize - requestOffset ) );
		auto copyStart = std::max( offset, requestOffset );
		auto copyEnd = std::min( objectEnd, requestOffset + requestSize );
		if ( copyEnd <= copyStart )
		{
			return;
		}

		auto objectOffset = copyStart - offset;
		auto copySize = copyEnd - copyStart;
		auto & layout = subresources[subresource];
		D3D11_MAPPED_SUBRESOURCE mapped{};

		if ( lock( *context, subresource, mapped ) == VK_SUCCESS )
		{
			if ( mapped.pData && mapped.DepthPitch > 0u && mapped.RowPitch > 0u )
			{
				auto src = static_cast< uint8_t const * >( mapped.pData ) + objectOffset;
				auto dst = data + copyStart;

				if ( mapped.DepthPitch == layout.SysMemSlicePitch || mapped.RowPitch == layout.SysMemPitch )
				{
					copySize = std::min( copySize, VkDeviceSize( mapped.DepthPitch ) - std::min( objectOffset, VkDeviceSize( mapped.DepthPitch ) ) );
					if ( copySize > 0u ) std::memcpy( dst, src, size_t( copySize ) );
				}
				else if ( layout.SysMemPitch > 0u )
				{
					auto mappedSteps = mapped.DepthPitch / mapped.RowPitch;
					auto systemSteps = layout.SysMemSlicePitch / layout.SysMemPitch;
					auto rowSize = std::min( mapped.RowPitch, layout.SysMemPitch );
					for ( UINT step = 0u; step < std::min( mappedSteps, systemSteps ); ++step )
					{
						auto rowOffset = VkDeviceSize( step ) * layout.SysMemPitch;
						if ( rowOffset >= copySize ) break;
						auto bytes = std::min( VkDeviceSize( rowSize ), copySize - rowOffset );
						std::memcpy( dst + rowOffset, static_cast< uint8_t const * >( mapped.pData ) + VkDeviceSize( step ) * mapped.RowPitch, size_t( bytes ) );
					}
				}
			}
			unlock( *context, subresource );
		}
	}

	//*********************************************************************************************

	class BufferDeviceMemory
	{
	public:
		BufferDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkDeviceSize bufferSize
			, VkBufferUsageFlags targets )
			: m_bufferTargets{ targets }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_buffer{ doCreateBuffer( device, bufferSize ) }
			, memory{ device, parent, m_buffer, offset, std::move( allocateInfo ), getBufferMapFlags( m_propertyFlags, m_bufferTargets ) }
		{
			memory.size = bufferSize;
			memory.subresources.push_back( { nullptr, UINT( bufferSize ), UINT( bufferSize ) } );
		}

		inline ID3D11Buffer * getBuffer()const
		{
			return m_buffer;
		}

	private:
		ID3D11Buffer * doCreateBuffer( VkDevice device
			, VkDeviceSize bufferSize )const
		{
			ID3D11Buffer * result{ nullptr };
			auto d3ddevice = get( device )->getDevice();

			if ( bufferSize == 0u || bufferSize > 0xFFFFFFFFULL )
			{
				throw ashes::Exception{ VK_ERROR_OUT_OF_DEVICE_MEMORY, "D3D11 buffer size exceeds UINT range" };
			}

			if ( auto size = UINT( bufferSize ); size > 0u )
			{
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = size;
				desc.Usage = getBufferUsage( m_propertyFlags, m_bufferTargets );
				desc.BindFlags = getBindFlags( m_bufferTargets );
				desc.CPUAccessFlags = getCpuBufferAccessFlags( m_propertyFlags, m_bufferTargets );
				desc.MiscFlags = ( checkFlag( m_bufferTargets, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT )
					? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
					: 0u );
				desc.StructureByteStride = 0;

				auto hr = d3ddevice->CreateBuffer( &desc, nullptr, &result );

				if ( !checkError( device, hr, "CreateBuffer" ) || !result )
				{
					throw ashes::Exception{ VK_ERROR_OUT_OF_DEVICE_MEMORY, "ID3D11Device::CreateBuffer failed" };
				}

				dxDebugName( result, Buffer );
			}

			return result;
		}

	private:
		VkBufferUsageFlags m_bufferTargets;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Buffer * m_buffer{ nullptr };

	public:
		ObjectMemory memory;
	};
	
	//*********************************************************************************************

	class Texture1DDeviceMemory
	{
	public:
		Texture1DDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkImageCreateInfo const & createInfo )
			: m_usage{ createInfo.usage }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_texture{ doCreateTexture( device, createInfo ) }
			, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
		{
			auto extent = getTexelBlockExtent( createInfo.format );
			auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
			memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

			for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
					auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
					auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
					auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
					memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
				}
			}
		}

		inline ID3D11Texture1D * getImage()const
		{
			return m_texture;
		}

	private:
		ID3D11Texture1D * doCreateTexture( VkDevice device
			, VkImageCreateInfo const & createInfo )const
		{
			ID3D11Texture1D * result{};
			getImageMapFlags( m_propertyFlags, m_usage );
			auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE1D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

			if ( HRESULT hr = d3ddevice->CreateTexture1D( &desc
					, nullptr
					, &result );
				checkError( device, hr, "CreateTexture1D" ) )
			{
				dxDebugName( result, Texture1D );
			}

			return result;
		}

		void doInitTexDesc( VkDevice device
			, VkImageCreateInfo const & createInfo
			, D3D11_TEXTURE1D_DESC & desc )const
		{
			desc.Width = createInfo.extent.width;
			desc.Format = getDxgiFormatGroup( getTextureFormat( createInfo.format ) );
			desc.ArraySize = createInfo.arrayLayers;
			desc.Usage = getImageUsage( m_propertyFlags, m_usage );
			desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
			desc.MipLevels = createInfo.mipLevels;
			desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture1D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};

	//*********************************************************************************************

	class Texture2DDeviceMemory
	{
	public:
		Texture2DDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkImageCreateInfo const & createInfo )
			: m_usage{ createInfo.usage }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_texture{ doCreateTexture( device, createInfo ) }
			, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
		{
			auto extent = getTexelBlockExtent( createInfo.format );
			auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
			memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

			for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
					auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
					auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
					auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
					memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
				}
			}
		}

		inline ID3D11Texture2D * getImage()const
		{
			return m_texture;
		}

	private:
		ID3D11Texture2D * doCreateTexture( VkDevice device
			, VkImageCreateInfo const & createInfo )const
		{
			ID3D11Texture2D * result{};
			auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE2D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

			if ( HRESULT hr = d3ddevice->CreateTexture2D( &desc
					, nullptr
					, &result );
				checkError( device, hr, "CreateTexture2D" ) )
			{
				dxDebugName( result, Texture2D );
			}

			return result;
		}

		void doInitTexDesc( VkDevice device
			, VkImageCreateInfo const & createInfo
			, D3D11_TEXTURE2D_DESC & desc )const
		{
			auto d3ddevice = get( device )->getDevice();
			desc.Width = createInfo.extent.width;
			desc.Height = createInfo.extent.height;
			desc.Format = getDxgiFormatGroup( getTextureFormat( createInfo.format ) );
			desc.ArraySize = createInfo.arrayLayers;
			desc.Usage = getImageUsage( m_propertyFlags, m_usage );
			desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
			desc.MipLevels = createInfo.mipLevels;
			desc.SampleDesc.Count = UINT( createInfo.samples );
			desc.SampleDesc.Quality = 0;
			desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( desc.SampleDesc.Count > 1 )
			{
				HRESULT hr = d3ddevice->CheckMultisampleQualityLevels( desc.Format
					, desc.SampleDesc.Count
					, &desc.SampleDesc.Quality );

				if ( hr == S_OK && desc.SampleDesc.Quality > 0 )
				{
					desc.SampleDesc.Quality--;
				}
			}

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture2D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};
	
	//*********************************************************************************************

	class Texture3DDeviceMemory
	{
	public:
		Texture3DDeviceMemory( VkDevice device
			, VkDeviceMemory parent
			, VkDeviceSize offset
			, VkMemoryAllocateInfo allocateInfo
			, VkImageCreateInfo const & createInfo )
			: m_usage{ createInfo.usage }
			, m_propertyFlags{ mem::getMemoryProperties( allocateInfo.memoryTypeIndex ) }
			, m_texture{ doCreateTexture( device, createInfo ) }
			, memory{ device, parent, m_texture, offset, std::move( allocateInfo ), getImageMapFlags( m_propertyFlags, m_usage ) }
		{
			auto extent = getTexelBlockExtent( createInfo.format );
			auto byteSize = getTexelBlockByteSize( extent, createInfo.format );
			memory.subresources.resize( size_t( createInfo.arrayLayers ) * createInfo.mipLevels );

			for ( uint32_t layer = 0u; layer < createInfo.arrayLayers; ++layer )
			{
				for ( uint32_t level = 0u; level < createInfo.mipLevels; ++level )
				{
					auto mipWidth = getSubresourceValue( createInfo.extent.width, level );
					auto mipHeight = getSubresourceValue( createInfo.extent.height, level );
					auto rowPitch = byteSize * mipWidth / ( extent.width * extent.height * extent.depth );
					auto depthPitch = rowPitch * mipHeight * extent.height / ( extent.width * extent.depth );
					memory.subresources[D3D11CalcSubresource( level, layer, createInfo.mipLevels )] = { nullptr, rowPitch, depthPitch };
				}
			}
		}

		inline ID3D11Texture3D * getImage()const
		{
			return m_texture;
		}

	private:
		ID3D11Texture3D * doCreateTexture( VkDevice device
			, VkImageCreateInfo const & createInfo )const
		{
			ID3D11Texture3D * result{};
			auto d3ddevice = get( device )->getDevice();
			D3D11_TEXTURE3D_DESC desc{};
			doInitTexDesc( device, createInfo, desc );

			if ( HRESULT hr = d3ddevice->CreateTexture3D( &desc
					, nullptr
					, &result );
				checkError( device, hr, "CreateTexture3D" ) )
			{
				dxDebugName( result, Texture3D );
			}

			return result;
		}

		void doInitTexDesc( VkDevice device
			, VkImageCreateInfo const & createInfo
			, D3D11_TEXTURE3D_DESC & desc )const
		{
			desc.Width = createInfo.extent.width;
			desc.Height = createInfo.extent.height;
			desc.Depth = createInfo.extent.depth;
			desc.Format = getDxgiFormatGroup( getTextureFormat( createInfo.format ) );
			desc.Usage = getImageUsage( m_propertyFlags, m_usage );
			desc.CPUAccessFlags = getCpuImageAccessFlags( m_propertyFlags, m_usage );
			desc.MipLevels = createInfo.mipLevels;
			desc.BindFlags = mem::getBindFlags( createInfo, m_propertyFlags );
			desc.MiscFlags = mem::getMiscFlags( createInfo );

			if ( get( device )->getFeatureLevel() < D3D_FEATURE_LEVEL_11_0 )
			{
				desc.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
			}
		}

	private:
		VkImageUsageFlags m_usage;
		VkMemoryPropertyFlags m_propertyFlags;
		ID3D11Texture3D * m_texture{ nullptr };

	public:
		ObjectMemory memory;
	};

	//*********************************************************************************************

	DeviceMemory::DeviceMemory( VkDevice device
		, VkMemoryAllocateInfo allocateInfo )
		: m_device{ device }
		, m_allocateInfo{ std::move( allocateInfo ) }
		, m_propertyFlags{ mem::getMemoryProperties( m_allocateInfo.memoryTypeIndex ) }
	{
		thread_local uint8_t defaultInitValue = 10u;

		if ( ashes::checkFlag( m_propertyFlags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) )
		{
			m_data.resize( m_allocateInfo.allocationSize, defaultInitValue );
			++defaultInitValue;
		}
	}

	DeviceMemory::~DeviceMemory()noexcept
	{
		if ( !m_objects.empty() )
		{
			onDestroy( get( this ) );
		}
	}

	VkResult DeviceMemory::bindToBuffer( VkBuffer buffer
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			BufferDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( buffer )->getSize(),
				get( buffer )->getUsage()
			};
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			auto context{ get( m_device )->getImmediateContext() };
			updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage1D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			Texture1DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			impl.memory.size = get( image )->getMemoryRequirements().size;
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			auto context{ get( m_device )->getImmediateContext() };
			updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage2D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			Texture2DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			impl.memory.size = get( image )->getMemoryRequirements().size;
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			auto context{ get( m_device )->getImmediateContext() };
			updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	VkResult DeviceMemory::bindToImage3D( VkImage image
		, VkDeviceSize memoryOffset
		, ObjectMemory *& objectMemory )
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;

		try
		{
			Texture3DDeviceMemory impl
			{
				m_device,
				get( this ),
				memoryOffset,
				m_allocateInfo,
				get( image )->getCreateInfo()
			};
			impl.memory.size = get( image )->getMemoryRequirements().size;
			m_objects.emplace_back( std::make_unique< ObjectMemory >( std::move( impl.memory ) ) );
			objectMemory = m_objects.back().get();
			auto context{ get( m_device )->getImmediateContext() };
			updateUpload( context, *objectMemory, 0ULL, WholeSize, 0u );
			result = VK_SUCCESS;
		}
		catch ( Exception & exc )
		{
			std::cerr << exc.what() << std::endl;
			result = exc.getResult();
		}
		catch ( std::exception & exc )
		{
			std::cerr << exc.what() << std::endl;
		}
		catch ( ... )
		{
			std::cerr << "Unknown error" << std::endl;
		}

		return result;
	}

	void DeviceMemory::updateUpload( DeviceContextLock const & context
		, ObjectMemory const & memory
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() )
		{
			memory.upload( context
				, m_data.data()
				, subresource
				, ( ( size == WholeSize && offset == 0u )
					? 0u
					: offset + memory.offset )
				, size );
		}
	}

	void DeviceMemory::updateDownload( DeviceContextLock const & context
		, ObjectMemory const & memory
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() )
		{
			memory.download( context
				, m_data.data()
				, subresource
				, ( ( size == WholeSize && offset == 0u )
					? 0u
					: offset + memory.offset )
				, size );
		}
	}

	void DeviceMemory::updateUpload( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() && !m_objects.empty() )
		{
			for ( auto & object : m_objects )
			{
				object->upload( context, m_data.data(), subresource, offset, size );
			}
			m_dirty = false;
		}
	}

	void DeviceMemory::updateDownload( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		if ( !m_data.empty() && !m_objects.empty() )
		{
			for ( auto & object : m_objects )
			{
				object->download( context, m_data.data(), subresource, offset, size );
			}
			m_dirty = false;
		}
	}

	void DeviceMemory::updateData( VkDeviceMemory src
		, VkDeviceSize srcOffset
		, VkDeviceSize dstOffset
		, VkDeviceSize size )
	{
		if ( !m_data.empty()
			&& !get( src )->m_data.empty()
			&& srcOffset <= get( src )->m_data.size()
			&& dstOffset <= m_data.size()
			&& size <= get( src )->m_data.size() - srcOffset
			&& size <= m_data.size() - dstOffset )
		{
			std::memcpy( &m_data[dstOffset], &get( src )->m_data[srcOffset], size_t( size ) );
		}
	}

	VkResult DeviceMemory::lock( VkDeviceSize offset
		, VkDeviceSize size
		, VkMemoryMapFlags flags
		, void ** data )const
	{
		assert( !m_mapped && "VkDeviceMemory should not be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		if ( offset >= m_allocateInfo.allocationSize ) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		size = ( size == WholeSize ? m_allocateInfo.allocationSize - offset : size );
		if ( size > m_allocateInfo.allocationSize - offset )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		*data = m_data.data() + offset;
		m_mappedOffset = offset;
		m_mappedSubresource = flags;
		m_mappedSize = size;
		m_mapped = *data != nullptr;
		m_dirty = true;
		return VK_SUCCESS;
	}

	VkResult DeviceMemory::flush( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size )const
	{
		assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		if ( offset >= m_allocateInfo.allocationSize ) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		size = ( size == WholeSize ? m_allocateInfo.allocationSize - offset : size );
		if ( size > m_allocateInfo.allocationSize - offset )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		updateUpload( context, offset, size, m_mappedSubresource );
		return VK_SUCCESS;
	}

	VkResult DeviceMemory::invalidate( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size )const
	{
		assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		if ( offset >= m_allocateInfo.allocationSize ) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		size = ( size == WholeSize ? m_allocateInfo.allocationSize - offset : size );
		if ( size > m_allocateInfo.allocationSize - offset )
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}

		m_dirty = true;
		updateDownload( context, offset, size, m_mappedSubresource );
		return VK_SUCCESS;
	}

	void DeviceMemory::unlock( DeviceContextLock const & context )const
	{
		assert( m_mapped && "VkDeviceMemory should be mapped" );
		assert( !m_data.empty() && "VkDeviceMemory should be mappable" );
		m_mapped = false;
		updateUpload( context, m_mappedOffset, m_mappedSize, m_mappedSubresource );
	}

	void DeviceMemory::upload( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		updateUpload( context, offset, size, subresource );
	}

	void DeviceMemory::download( DeviceContextLock const & context
		, VkDeviceSize offset
		, VkDeviceSize size
		, UINT subresource )const
	{
		updateDownload( context, offset, size, subresource );
	}

	//*********************************************************************************************
}
