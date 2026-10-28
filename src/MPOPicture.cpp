/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "MPOPicture.h"

#include "../lib/TinyEXIF/TinyEXIF.h"

#include <csetjmp>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <vector>
#include <kodi/Filesystem.h>

namespace
{

thread_local std::jmp_buf s_jpegEscape;

void MpoFatalError(j_common_ptr cinfo)
{
  char message[JMSG_LENGTH_MAX] = {};
  (*cinfo->err->format_message)(cinfo, message);
  kodi::Log(ADDON_LOG_ERROR, "libjpeg: %s", message);
  std::longjmp(s_jpegEscape, 1);
}

} // namespace

MPOPicture::MPOPicture(const kodi::addon::IInstanceInfo& instance)
  : CInstanceImageDecoder(instance)
{
}

MPOPicture::~MPOPicture()
{
  if (m_allocated)
    mpo_destroy_decompress(&m_mpoinfo);
  m_allocated = false;
}

bool MPOPicture::SupportsFile(const std::string& file)
{
  kodi::vfs::CFile fileData;
  if (!fileData.OpenFile(file))
    return false;

  std::vector<uint8_t> buffer(fileData.GetLength());
  fileData.Read(buffer.data(), buffer.size());

  mpo_decompress_struct mpoinfo;
  mpo_create_decompress(&mpoinfo);
  mpo_decompress_error_exit(&mpoinfo, MpoFatalError);
  if (setjmp(s_jpegEscape))
  {
    mpo_destroy_decompress(&mpoinfo);
    return false;
  }

  mpo_mem_src(&mpoinfo, buffer.data(), buffer.size());
  const bool ret = mpo_read_header(&mpoinfo);
  mpo_destroy_decompress(&mpoinfo);
  return ret;
}

bool MPOPicture::ReadTag(const std::string& file, kodi::addon::ImageDecoderInfoTag& tag)
{
  kodi::vfs::CFile fileData;
  if (!fileData.OpenFile(file))
    return false;

  const size_t length = fileData.GetLength();

  std::vector<uint8_t> buffer(length);
  fileData.Read(buffer.data(), buffer.size());

  TinyEXIF::EXIFInfo imageEXIF;
  if (imageEXIF.parseFrom(buffer.data(), buffer.size()) != 0)
    return false;

  tag.SetWidth(imageEXIF.ImageWidth);
  tag.SetHeight(imageEXIF.ImageHeight);

  switch (imageEXIF.Orientation)
  {
    case 3:
      tag.SetOrientation(ADDON_IMG_ORIENTATION_ROTATE_180_CCW);
      break;
    case 5:
      tag.SetOrientation(ADDON_IMG_ORIENTATION_ROTATE_90_CCW);
      break;
    case 6:
      tag.SetOrientation(ADDON_IMG_ORIENTATION_ROTATE_270_CCW);
      break;
    default:
      tag.SetOrientation(ADDON_IMG_ORIENTATION_NONE);
      break;
  }

  std::string dateTime;
  if (!imageEXIF.DateTimeOriginal.empty())
    dateTime = imageEXIF.DateTimeOriginal;
  else if (!imageEXIF.DateTime.empty())
    dateTime = imageEXIF.DateTime;
  else if (!imageEXIF.DateTimeDigitized.empty())
    dateTime = imageEXIF.DateTimeDigitized;
  if (dateTime.size() == 19)
  {
    struct tm tm = {0};
    tm.tm_year = atoi(dateTime.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = atoi(dateTime.substr(5, 2).c_str()) - 1;
    tm.tm_mday = atoi(dateTime.substr(8, 2).c_str());
    tm.tm_hour = atoi(dateTime.substr(11, 2).c_str());
    tm.tm_min = atoi(dateTime.substr(14, 2).c_str());
    tm.tm_sec = atoi(dateTime.substr(17, 2).c_str());
    tm.tm_isdst = -1;
    tag.SetTimeCreated(mktime(&tm));
  }
  tag.SetDistance(imageEXIF.SubjectDistance);
  tag.SetISOSpeed(imageEXIF.ISOSpeedRatings);
  tag.SetFocalLength(imageEXIF.FocalLength);
  tag.SetFocalLengthIn35mmFormat(imageEXIF.LensInfo.FocalLengthIn35mm);
  tag.SetCameraManufacturer(imageEXIF.Make);
  tag.SetCameraModel(imageEXIF.Model);
  tag.SetExposureBias(imageEXIF.ExposureBiasValue);
  tag.SetExposureTime(imageEXIF.ExposureTime);
  tag.SetExposureProgram(static_cast<ADDON_IMG_EXPOSURE_PROGRAM>(imageEXIF.ExposureProgram));
  tag.SetMeteringMode(static_cast<ADDON_IMG_METERING_MODE>(imageEXIF.MeteringMode));
  tag.SetApertureFNumber(imageEXIF.FNumber);
  tag.SetFlashUsed(static_cast<ADDON_IMG_FLASH_TYPE>(imageEXIF.Flash));
  tag.SetLightSource(static_cast<ADDON_IMG_LIGHT_SOURCE>(imageEXIF.LightSource));
  tag.SetDescription(imageEXIF.ImageDescription);
  tag.SetDigitalZoomRatio(imageEXIF.LensInfo.DigitalZoomRatio);

  if (imageEXIF.GeoLocation.hasLatLon() && imageEXIF.GeoLocation.hasAltitude() &&
      isalpha(imageEXIF.GeoLocation.LatComponents.direction))
  {
    float lat[] = {static_cast<float>(imageEXIF.GeoLocation.LatComponents.degrees),
                   static_cast<float>(imageEXIF.GeoLocation.LatComponents.minutes),
                   static_cast<float>(imageEXIF.GeoLocation.LatComponents.seconds)};

    float lon[] = {static_cast<float>(imageEXIF.GeoLocation.LonComponents.degrees),
                   static_cast<float>(imageEXIF.GeoLocation.LonComponents.minutes),
                   static_cast<float>(imageEXIF.GeoLocation.LonComponents.seconds)};
    tag.SetGPSInfo(true, imageEXIF.GeoLocation.LatComponents.direction, lat,
                   imageEXIF.GeoLocation.LonComponents.direction, lon,
                   imageEXIF.GeoLocation.AltitudeRef, imageEXIF.GeoLocation.Altitude);
  }

  return true;
}

bool MPOPicture::LoadImageFromMemory(const std::string& mimetype,
                                     const uint8_t* buffer,
                                     size_t bufSize,
                                     unsigned int& width,
                                     unsigned int& height)
{
  m_data.resize(bufSize);
  std::copy(buffer, buffer + bufSize, m_data.begin());
  mpo_create_decompress(&m_mpoinfo);
  mpo_decompress_error_exit(&m_mpoinfo, MpoFatalError);
  if (setjmp(s_jpegEscape))
  {
    mpo_destroy_decompress(&m_mpoinfo);
    return false;
  }

  mpo_mem_src(&m_mpoinfo, m_data.data(), m_data.size());
  if (!mpo_read_header(&m_mpoinfo))
  {
    mpo_destroy_decompress(&m_mpoinfo);
    return false;
  }
  m_allocated = true;
  m_images = mpo_get_number_images(&m_mpoinfo);

  // Zwinge das Ausgabebild fest auf 1920x1080 (Full HD Half-SBS)
  m_width = width = 1920;
  m_height = height = 1080;

  return true;
}

bool MPOPicture::Decode(uint8_t* pixels,
                        size_t pixelBufferSize,
                        unsigned int width,
                        unsigned int height,
                        unsigned int pitch,
                        ADDON_IMG_FMT format)
{
  if (format != ADDON_IMG_FMT_A8R8G8B8)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s: Unsupported target format (%d)", __func__,
              static_cast<int>(format));
    return false;
  }

  if (setjmp(s_jpegEscape))
    return false;

  // Initialisiere das Ziel-Canvas komplett mit Schwarz (0x00)
  std::memset(pixels, 0, pixelBufferSize);

  size_t image = 0;
  while (image < m_images && image < 2) // Wir verarbeiten max. 2 Augen (3D)
  {
    mpo_start_decompress(&m_mpoinfo);
    
    int srcW = m_mpoinfo.cinfo.cinfo.output_width;
    int srcH = m_mpoinfo.cinfo.cinfo.output_height;
    int row_stride = srcW * m_mpoinfo.cinfo.cinfo.output_components;

    // Lese alle Scanlines des Quellbildes in einen temporären Puffer
    std::vector<uint8_t> eyeBuffer(srcW * srcH * 3);
    size_t lines = 0;
    while (lines < static_cast<size_t>(srcH))
    {
      JSAMPARRAY buffer = (*m_mpoinfo.cinfo.cinfo.mem->alloc_sarray)(
          (j_common_ptr)&m_mpoinfo.cinfo, JPOOL_IMAGE, row_stride, srcH);
      size_t nl = mpo_read_scanlines(&m_mpoinfo, buffer, srcH - lines);
      for (size_t l = 0; l < nl; ++l)
      {
        std::memcpy(eyeBuffer.data() + (lines + l) * row_stride, buffer[l], row_stride);
      }
      lines += nl;
    }
    mpo_finish_decompress(&m_mpoinfo);

    // Berechne Half-SBS Stauchung & Zentrierung (Pillarbarmatching)
    float scaleY = static_cast<float>(height) / srcH;
    int propWidth = static_cast<int>(srcW * scaleY);
    int targetEyeWidth = std::min(propWidth / 2, static_cast<int>(width / 2));
    int marginX = (width / 2 - targetEyeWidth) / 2;
    int offsetX = (image == 1 ? (width / 2) : 0) + marginX;

    // Bilineares Downsampling direkt in Kodis Zielpuffer (BGRA / A8R8G8B8)
    for (unsigned int y = 0; y < height; ++y)
    {
      float srcY = y / scaleY;
      int y0 = static_cast<int>(srcY);
      int y1 = std::min(y0 + 1, srcH - 1);
      float dy = srcY - y0;

      for (int x = 0; x < targetEyeWidth; ++x)
      {
        float srcX = x * (static_cast<float>(srcW) / targetEyeWidth);
        int x0 = static_cast<int>(srcX);
        int x1 = std::min(x0 + 1, srcW - 1);
        float dx = srcX - x0;

        int dstX = offsetX + x;
        uint8_t* dst = pixels + y * pitch + dstX * 4;

        // Sample RGB Kanäle aus dem Quell-Eye
        for (int c = 0; c < 3; ++c)
        {
          float p00 = eyeBuffer[(y0 * srcW + x0) * 3 + c];
          float p10 = eyeBuffer[(y0 * srcW + x1) * 3 + c];
          float p01 = eyeBuffer[(y1 * srcW + x0) * 3 + c];
          float p11 = eyeBuffer[(y1 * srcW + x1) * 3 + c];

          float top = p00 + dx * (p10 - p00);
          float bottom = p01 + dx * (p11 - p01);
          float val = top + dy * (bottom - top);

          // Kodis A8R8G8B8 erwartet B-G-R-A Reihenfolge im Speicher
          if (c == 0) dst[2] = static_cast<uint8_t>(val); // R
          if (c == 1) dst[1] = static_cast<uint8_t>(val); // G
          if (c == 2) dst[0] = static_cast<uint8_t>(val); // B
        }
        dst[3] = 0xff; // Alpha voll deckend
      }
    }

    ++image;
  }

  return true;
}

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() = default;
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    hdl = new MPOPicture(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CMyAddon)
