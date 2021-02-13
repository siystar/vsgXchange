#include "ReaderWriter_ktx.h"

#include <vsg/core/Exception.h>
#include <vsg/state/DescriptorImage.h>

#include <ktx.h>
#include <ktxvulkan.h>
#include <texture.h>
#include <vk_format.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace
{

    template<typename T>
    vsg::ref_ptr<vsg::Data> createImage(uint32_t arrayDimensions, uint32_t width, uint32_t height, uint32_t depth, uint8_t* data, vsg::Data::Layout layout)
    {
        switch(arrayDimensions)
        {
            case 1: return vsg::Array<T>::create(width, reinterpret_cast<T*>(data), layout);
            case 2: return vsg::Array2D<T>::create(width, height, reinterpret_cast<T*>(data), layout);
            case 3: return vsg::Array3D<T>::create(width, height, depth, reinterpret_cast<T*>(data), layout);
            default : return {};
        }
    }

    vsg::ref_ptr<vsg::Data> readKtx(ktxTexture* texture, const vsg::Path& /*filename*/)
    {
        uint32_t width = texture->baseWidth;
        uint32_t height = texture->baseHeight;
        uint32_t depth = texture->baseDepth;
        const auto numMipMaps = texture->numLevels;
        const auto numLayers = texture->numLayers;
        const auto textureData = ktxTexture_GetData(texture);
        auto valueSize = ktxTexture_GetElementSize(texture);
        const auto format = ktxTexture_GetVkFormat(texture);

        ktxFormatSize formatSize;
        vkGetFormatSize( format, &formatSize );

        if (formatSize.blockSizeInBits != valueSize*8)
        {
            throw vsg::Exception{"Mismatched ktxFormatSize.blockSize and ktxTexture_GetElementSize(texture)."};
        }

        vsg::Data::Layout layout;
        layout.format = format;
        layout.blockWidth = texture->_protected->_formatSize.blockWidth;
        layout.blockHeight = texture->_protected->_formatSize.blockHeight;
        layout.blockDepth = texture->_protected->_formatSize.blockDepth;
        layout.maxNumMipmaps = numMipMaps;
        layout.origin = static_cast<uint8_t>(((texture->orientation.x == KTX_ORIENT_X_RIGHT) ? 0 : 1) |
                                             ((texture->orientation.y == KTX_ORIENT_Y_DOWN) ? 0 : 2) |
                                             ((texture->orientation.z == KTX_ORIENT_Z_OUT) ? 0 : 4));

        width /= layout.blockWidth;
        height /= layout.blockHeight;
        depth /= layout.blockDepth;

        // compute the textureSize.
        size_t textureSize = 0;
        {
            auto mipWidth = width;
            auto mipHeight = height;
            auto mipDepth = depth;

            for (uint32_t level = 0; level < numMipMaps; ++level)
            {
                const auto faceSize = std::max(mipWidth * mipHeight * mipDepth * valueSize, valueSize);
                textureSize += faceSize;

                if (mipWidth > 1) mipWidth /= 2;
                if (mipHeight > 1) mipHeight /= 2;
                if (mipDepth > 1) mipDepth /= 2;
            }
            textureSize *= (texture->numLayers * texture->numFaces);
        }

        // copy the data and repack into ordered assumed by VSG
        uint8_t* copiedData = new uint8_t[textureSize];

        size_t offset = 0;

        auto mipWidth = width;
        auto mipHeight = height;
        auto mipDepth = depth;

        for (uint32_t level = 0; level < numMipMaps; ++level)
        {
            const auto faceSize = std::max(mipWidth * mipHeight * mipDepth * valueSize, valueSize);
            for (uint32_t layer = 0; layer < texture->numLayers; ++layer)
            {
                for (uint32_t face = 0; face < texture->numFaces; ++face)
                {
                    if (ktx_size_t ktxOffset = 0; ktxTexture_GetImageOffset(texture, level, layer, face, &ktxOffset) == KTX_SUCCESS)
                    {
                        std::memcpy(copiedData + offset, textureData + ktxOffset, faceSize);
                    }

                    offset += faceSize;
                }
            }
            if (mipWidth > 1) mipWidth /= 2;
            if (mipHeight > 1) mipHeight /= 2;
            if (mipDepth > 1) mipDepth /= 2;
        }

        uint32_t arrayDimensions = 0;
        switch (texture->numDimensions)
        {
            case 1:
                layout.imageViewType = (numLayers == 1) ? VK_IMAGE_VIEW_TYPE_1D :VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                arrayDimensions = (numLayers == 1) ? 1 : 2;
                height = numLayers;
                break;

            case 2:
                if (texture->isCubemap)
                {
                    layout.imageViewType = (numLayers == 1) ? VK_IMAGE_VIEW_TYPE_CUBE :VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                    arrayDimensions = 3;
                    depth = 6 * numLayers;
                }
                else
                {
                    layout.imageViewType = (numLayers == 1) ? VK_IMAGE_VIEW_TYPE_2D :VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                    arrayDimensions = (numLayers == 1) ? 2 : 3;
                    depth = numLayers;
                }
                break;

            case 3:
                layout.imageViewType = VK_IMAGE_VIEW_TYPE_3D;
                arrayDimensions = 3;
                break;

            default:
                throw vsg::Exception{"Invalid number of dimensions."};
        }

        // create the VSG compressed image objects
        if (texture->isCompressed)
        {
            switch(valueSize)
            {
                case 8 : return createImage<vsg::block64>(arrayDimensions, width, height, depth, copiedData, layout);
                case 16 : return createImage<vsg::block128>(arrayDimensions, width, height, depth, copiedData, layout);
                default: throw vsg::Exception{"Unsupported compressed format."};
            }
        }

        // create the VSG uncompressed image objects
        switch(valueSize)
        {
            case 1:
                // int8_t or uint8_t
                return createImage<std::uint8_t>(arrayDimensions, width, height, depth, copiedData, layout);
            case 2:
                // short, ushort, ubvec2, bvec2
                return createImage<std::uint16_t>(arrayDimensions, width, height, depth, copiedData, layout);
            case 3:
                // ubvec3 or bcec3
                return createImage<vsg::ubvec3>(arrayDimensions, width, height, depth, copiedData, layout);
            case 4:
                // float, int, uint, usvec2, svec2, ubvec4, bvec4
                return createImage<vsg::ubvec4>(arrayDimensions, width, height, depth, copiedData, layout);
            case 8:
                // double, vec2, ivec4, uivec4, svec4, uvec4
                return createImage<vsg::usvec4>(arrayDimensions, width, height, depth, copiedData, layout);
            case 16:
                // dvec2, vec4, ivec4, uivec4
                return createImage<vsg::vec4>(arrayDimensions, width, height, depth, copiedData, layout);
            default:
                throw vsg::Exception{"Unsupported valueSize."};
        }

        return {};
    }

} // namespace

using namespace vsgXchange;

ReaderWriter_ktx::ReaderWriter_ktx() :
    _supportedExtensions{"ktx", "ktx2"}
{
}

vsg::ref_ptr<vsg::Object> ReaderWriter_ktx::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    if (const auto ext = vsg::lowerCaseFileExtension(filename); _supportedExtensions.count(ext) == 0)
        return {};

    vsg::Path filenameToUse = findFile(filename, options);
    if (filenameToUse.empty()) return {};

    if (ktxTexture * texture{nullptr}; ktxTexture_CreateFromNamedFile(filenameToUse.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) == KTX_SUCCESS)
    {
        vsg::ref_ptr<vsg::Data> data;
        try
        {
            data = readKtx(texture, filename);
        }
        catch(const vsg::Exception& ve)
        {
            std::cout<<"ReaderWriter_ktx::read("<<filenameToUse<<") failed : "<<ve.message<<std::endl;
        }

        ktxTexture_Destroy(texture);

        return data;
    }

    return {};
}

vsg::ref_ptr<vsg::Object> ReaderWriter_ktx::read(std::istream& fin, vsg::ref_ptr<const vsg::Options> options) const
{
    if (_supportedExtensions.count(options->extensionHint) == 0)
        return {};

    std::string buffer(1 << 16, 0); // 64kB
    std::string input;

    while (!fin.eof())
    {
        fin.read(&buffer[0], buffer.size());
        const auto bytes_readed = fin.gcount();
        input.append(&buffer[0], bytes_readed);
    }

    if (ktxTexture * texture{nullptr}; ktxTexture_CreateFromMemory((const ktx_uint8_t*)input.data(), input.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) == KTX_SUCCESS)
    {
        vsg::ref_ptr<vsg::Data> data;
        try
        {
            data = readKtx(texture, "");
        }
        catch(const vsg::Exception& ve)
        {
            std::cout<<"ReaderWriter_ktx::read(std::istream&) failed : "<<ve.message<<std::endl;
        }

        ktxTexture_Destroy(texture);

        return data;
    }

    return {};
}
