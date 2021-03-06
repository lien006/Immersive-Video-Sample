/*
 * Copyright (c) 2019, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

//!
//! \file:   VideoStream.cpp
//! \brief:  Video stream class implementation
//!
//! Created on April 30, 2019, 6:04 AM
//!

#include "VideoStream.h"
#include "AvcNaluParser.h"
#include "HevcNaluParser.h"

VCD_NS_BEGIN

VideoStream::VideoStream()
{
    m_streamIdx = 0;
    m_codecId = CODEC_ID_H265;
    m_width = 0;
    m_height = 0;
    m_tileInRow = 0;
    m_tileInCol = 0;
    m_tilesInfo = NULL;
    m_projType = 0;
    m_frameRate.num = 0;
    m_frameRate.den = 0;
    m_bitRate = 0;

    m_srcRwpk = NULL;
    m_srcCovi = NULL;

    m_videoSegInfoGen = NULL;
    m_currFrameInfo = NULL;

    m_360scvpParam = NULL;
    m_360scvpHandle = NULL;
    m_naluParser = NULL;
    m_isEOS = false;
}

VideoStream::~VideoStream()
{
    if (m_srcRwpk)
    {
        DELETE_ARRAY(m_srcRwpk->rectRegionPacking);

        delete m_srcRwpk;
        m_srcRwpk = NULL;
    }

    if (m_srcCovi)
    {
        DELETE_ARRAY(m_srcCovi->sphereRegions);

        delete m_srcCovi;
        m_srcCovi = NULL;
    }

    if (m_tilesInfo)
    {
        uint16_t tilesNum = m_tileInRow * m_tileInCol;
        for (uint16_t tileIdx = 0; tileIdx < tilesNum; tileIdx++)
        {
            DELETE_MEMORY(m_tilesInfo[tileIdx].tileNalu);
        }

        delete[] m_tilesInfo;
        m_tilesInfo = NULL;
    }

    DELETE_MEMORY(m_videoSegInfoGen);

    std::list<FrameBSInfo*>::iterator it1;
    for (it1 = m_frameInfoList.begin(); it1 != m_frameInfoList.end();)
    {
        FrameBSInfo *frameInfo = *it1;
        if (frameInfo)
        {
            DELETE_ARRAY(frameInfo->data);

            delete frameInfo;
            frameInfo = NULL;
        }

        it1 = m_frameInfoList.erase(it1);
    }
    m_frameInfoList.clear();

    std::list<FrameBSInfo*>::iterator it2;
    for (it2 = m_framesToOneSeg.begin(); it2 != m_framesToOneSeg.end();)
    {
        FrameBSInfo *frameInfo = *it2;
        if (frameInfo)
        {
            DELETE_ARRAY(frameInfo->data);

            delete frameInfo;
            frameInfo = NULL;
        }

        it2 = m_framesToOneSeg.erase(it2);
    }
    m_framesToOneSeg.clear();

    DELETE_MEMORY(m_360scvpParam);

    if (m_360scvpHandle)
    {
        I360SCVP_unInit(m_360scvpHandle);
    }

    DELETE_MEMORY(m_naluParser);
}

int32_t VideoStream::ParseHeader()
{
    m_naluParser->ParseHeaderData();
    m_width = m_naluParser->GetSrcWidth();
    m_height = m_naluParser->GetSrcHeight();
    m_tileInRow = m_naluParser->GetTileInRow();
    m_tileInCol = m_naluParser->GetTileInCol();
    m_projType = m_naluParser->GetProjectionType();

    uint16_t tilesNum = m_tileInRow * m_tileInCol;
    m_tilesInfo = new TileInfo[tilesNum];
    if (!m_tilesInfo)
        return OMAF_ERROR_NULL_PTR;

    for (uint16_t tileIdx = 0; tileIdx < tilesNum; tileIdx++)
    {
        m_naluParser->GetTileInfo(tileIdx, &(m_tilesInfo[tileIdx]));
        m_tilesInfo[tileIdx].tileNalu = new Nalu;
        if (!(m_tilesInfo[tileIdx].tileNalu))
            return OMAF_ERROR_NULL_PTR;
    }

    return ERROR_NONE;
}

int32_t VideoStream::FillRegionWisePacking()
{
    if (!m_srcRwpk)
        return OMAF_ERROR_NULL_PTR;

    if (!m_tilesInfo)
        return OMAF_ERROR_NULL_PTR;

    m_srcRwpk->constituentPicMatching = 0;
    m_srcRwpk->numRegions             = m_tileInRow * m_tileInCol;
    m_srcRwpk->projPicWidth           = m_width;
    m_srcRwpk->projPicHeight          = m_height;
    m_srcRwpk->packedPicWidth         = m_width;
    m_srcRwpk->packedPicHeight        = m_height;

    m_srcRwpk->rectRegionPacking      = new RectangularRegionWisePacking[m_srcRwpk->numRegions];
    if (!(m_srcRwpk->rectRegionPacking))
        return OMAF_ERROR_NULL_PTR;

    for (uint8_t regionIdx = 0; regionIdx < m_srcRwpk->numRegions; regionIdx++)
    {
        RectangularRegionWisePacking *rectRwpk = &(m_srcRwpk->rectRegionPacking[regionIdx]);
        TileInfo *tileInfo                     = &(m_tilesInfo[regionIdx]);

        memset(rectRwpk, 0, sizeof(RectangularRegionWisePacking));
        rectRwpk->transformType = 0;
        rectRwpk->guardBandFlag = 0;
        rectRwpk->projRegWidth  = tileInfo->tileWidth;
        rectRwpk->projRegHeight = tileInfo->tileHeight;
        rectRwpk->projRegLeft   = tileInfo->horizontalPos;
        rectRwpk->projRegTop    = tileInfo->verticalPos;

        rectRwpk->packedRegWidth  = tileInfo->tileWidth;
        rectRwpk->packedRegHeight = tileInfo->tileHeight;
        rectRwpk->packedRegLeft   = tileInfo->horizontalPos;
        rectRwpk->packedRegTop    = tileInfo->verticalPos;

        rectRwpk->leftGbWidth          = 0;
        rectRwpk->rightGbWidth         = 0;
        rectRwpk->topGbHeight          = 0;
        rectRwpk->bottomGbHeight       = 0;
        rectRwpk->gbNotUsedForPredFlag = true;
        rectRwpk->gbType0              = 0;
        rectRwpk->gbType1              = 0;
        rectRwpk->gbType2              = 0;
        rectRwpk->gbType3              = 0;
    }

    return ERROR_NONE;
}

int32_t VideoStream::FillContentCoverage()
{
    if (!m_srcCovi)
        return OMAF_ERROR_NULL_PTR;

    if (!m_srcRwpk)
        return OMAF_ERROR_NULL_PTR;

    if (m_projType == 0) //ERP projection type
    {
        m_srcCovi->coverageShapeType = 1;// TwoAzimuthAndTwoElevationCircles
    }
    else
    {
        m_srcCovi->coverageShapeType = 0; //FourGreatCircles
    }

    m_srcCovi->numRegions          = m_tileInRow * m_tileInCol;
    m_srcCovi->viewIdcPresenceFlag = false;
    m_srcCovi->defaultViewIdc      = 0;

    m_srcCovi->sphereRegions = new SphereRegion[m_srcCovi->numRegions];
    if (!(m_srcCovi->sphereRegions))
        return OMAF_ERROR_NULL_PTR;

    // Fill sphere region information for each tile
    for (uint8_t regionIdx = 0; regionIdx < m_srcCovi->numRegions; regionIdx++)
    {
        SphereRegion *sphereRegion             = &(m_srcCovi->sphereRegions[regionIdx]);
        RectangularRegionWisePacking *rectRwpk = &(m_srcRwpk->rectRegionPacking[regionIdx]);

        memset(sphereRegion, 0, sizeof(SphereRegion));
        sphereRegion->viewIdc         = 0; //doesn't take effect when viewIdcPresenceFlag is 0
        sphereRegion->centreAzimuth   = (int32_t)((((m_width / 2) - (float)(rectRwpk->projRegLeft + rectRwpk->projRegWidth / 2)) * 360 * 65536) / m_width);
        sphereRegion->centreElevation = (int32_t)((((m_height / 2) - (float)(rectRwpk->projRegTop + rectRwpk->projRegHeight / 2)) * 180 * 65536) / m_height);
        sphereRegion->centreTilt      = 0;
        sphereRegion->azimuthRange    = (uint32_t)((rectRwpk->projRegWidth * 360.f * 65536) / m_width);
        sphereRegion->elevationRange  = (uint32_t)((rectRwpk->projRegHeight * 180.f * 65536) / m_height);
        sphereRegion->interpolate     = 0;
    }

    return ERROR_NONE;
}

int32_t VideoStream::Initialize(
    uint8_t streamIdx,
    BSBuffer *bs,
    InitialInfo *initInfo)
{
    if (!bs || !initInfo)
        return OMAF_ERROR_NULL_PTR;

    m_srcRwpk = new RegionWisePacking;
    if (!m_srcRwpk)
        return OMAF_ERROR_NULL_PTR;

    m_srcCovi = new ContentCoverage;
    if (!m_srcCovi)
        return OMAF_ERROR_NULL_PTR;

    m_streamIdx = streamIdx;

    m_codecId = bs->codecId;
    m_frameRate = bs->frameRate;
    m_bitRate = bs->bitRate;

    m_360scvpParam = new param_360SCVP;
    if (!m_360scvpParam)
        return OMAF_ERROR_NULL_PTR;

    memset(m_360scvpParam, 0, sizeof(param_360SCVP));

    m_360scvpParam->usedType                         = E_PARSER_ONENAL;
    m_360scvpParam->pInputBitstream                  = bs->data;
    m_360scvpParam->inputBitstreamLen                = bs->dataSize;

    m_360scvpHandle = I360SCVP_Init(m_360scvpParam);
    if (!m_360scvpHandle)
        return OMAF_ERROR_SCVP_INIT_FAILED;

    if (m_codecId == 0) //CODEC_ID_H264
    {
        m_naluParser = new AvcNaluParser(m_360scvpHandle, m_360scvpParam);
        if (!m_naluParser)
            return OMAF_ERROR_NULL_PTR;
    } else if (m_codecId == 1) { //CODEC_ID_H265
        m_naluParser = new HevcNaluParser(m_360scvpHandle, m_360scvpParam);
        if (!m_naluParser)
            return OMAF_ERROR_NULL_PTR;
    } else {
        return OMAF_ERROR_UNDEFINED_OPERATION;
    }

    int32_t ret = ParseHeader();
    if (ret)
        return ret;

    m_videoSegInfoGen = new VideoSegmentInfoGenerator(
                                bs, initInfo, m_streamIdx,
                                m_width, m_height,
                                m_tileInRow, m_tileInCol);
    if (!m_videoSegInfoGen)
        return OMAF_ERROR_NULL_PTR;

    ret = m_videoSegInfoGen->Initialize(m_tilesInfo);
    if (ret)
        return ret;

    ret = FillRegionWisePacking();
    if (ret)
        return ret;

    ret = FillContentCoverage();
    if (ret)
        return ret;

    //uint32_t tilesNum = m_tileInRow * m_tileInCol;
    //m_trackSegCtxs = new TrackSegmentCtx[tilesNum];
    //if (!m_trackSegCtxs);
        //return OMAF_ERROR_NULL_PTR;

    return ERROR_NONE;
}

int32_t VideoStream::AddFrameInfo(FrameBSInfo *frameInfo)
{
    if (!frameInfo || !(frameInfo->data))
        return OMAF_ERROR_NULL_PTR;

    if (!frameInfo->dataSize)
        return OMAF_ERROR_DATA_SIZE;

    FrameBSInfo *newFrameInfo = new FrameBSInfo;
    if (!newFrameInfo)
        return OMAF_ERROR_NULL_PTR;

    memset(newFrameInfo, 0, sizeof(FrameBSInfo));

    uint8_t *localData = new uint8_t[frameInfo->dataSize];
    if (!localData)
    {
        delete newFrameInfo;
        newFrameInfo = NULL;
        return OMAF_ERROR_NULL_PTR;
    }
    memcpy(localData, frameInfo->data, frameInfo->dataSize);

    newFrameInfo->data = localData;
    newFrameInfo->dataSize = frameInfo->dataSize;
    newFrameInfo->pts = frameInfo->pts;
    newFrameInfo->isKeyFrame = frameInfo->isKeyFrame;

    m_frameInfoList.push_back(newFrameInfo);

    return ERROR_NONE;
}

void VideoStream::SetCurrFrameInfo()
{
    if (m_frameInfoList.size() > 0)
    {
        m_currFrameInfo = m_frameInfoList.front();
        m_frameInfoList.pop_front();
    }
}

int32_t VideoStream::UpdateTilesNalu()
{
    if (!m_currFrameInfo)
        return OMAF_ERROR_NULL_PTR;

    uint16_t tilesNum = m_tileInRow * m_tileInCol;
    int32_t ret = m_naluParser->ParseSliceNalu(m_currFrameInfo->data, m_currFrameInfo->dataSize, tilesNum, m_tilesInfo);
    if (ret)
        return ret;

    return ERROR_NONE;
}

TileInfo* VideoStream::GetAllTilesInfo()
{
    return m_tilesInfo;
}

FrameBSInfo* VideoStream::GetCurrFrameInfo()
{
    return m_currFrameInfo;
}

void VideoStream::DestroyCurrSegmentFrames()
{
    std::list<FrameBSInfo*>::iterator it;
    for (it = m_framesToOneSeg.begin(); it != m_framesToOneSeg.end(); )
    {
        FrameBSInfo *frameInfo = *it;
        if (frameInfo)
        {
            DELETE_ARRAY(frameInfo->data);
            delete frameInfo;
            frameInfo = NULL;
        }

        //m_framesToOneSeg.erase(it++);
        it = m_framesToOneSeg.erase(it);

    }
    m_framesToOneSeg.clear();
}

void VideoStream::DestroyCurrFrameInfo()
{
    if (m_currFrameInfo)
    {
        DELETE_ARRAY(m_currFrameInfo->data);

        delete m_currFrameInfo;
        m_currFrameInfo = NULL;
    }
}

Nalu* VideoStream::GetVPSNalu()
{
    if (m_codecId == CODEC_ID_H265)
    {
        return ((HevcNaluParser*)m_naluParser)->GetVPSNalu();
    }
    else
        return NULL;
}

Nalu* VideoStream::GetSPSNalu()
{
    return m_naluParser->GetSPSNalu();
}

Nalu* VideoStream::GetPPSNalu()
{
    return m_naluParser->GetPPSNalu();
}

VCD_NS_END
