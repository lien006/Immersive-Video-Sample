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
//! \file:   DefaultSegmentation.cpp
//! \brief:  Default segmentation class implementation
//!
//! Created on April 30, 2019, 6:04 AM
//!

#include "DefaultSegmentation.h"
#include "streamsegmenter/rational.hpp"

#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <sys/time.h>

VCD_NS_BEGIN

DefaultSegmentation::~DefaultSegmentation()
{
    std::map<MediaStream*, TrackSegmentCtx*>::iterator itTrackCtx;
    for (itTrackCtx = m_streamSegCtx.begin();
        itTrackCtx != m_streamSegCtx.end();
        itTrackCtx++)
    {
        TrackSegmentCtx *trackSegCtxs = itTrackCtx->second;
        MediaStream *stream = itTrackCtx->first;
        VideoStream *vs = (VideoStream*)stream;
        uint32_t tilesNum = vs->GetTileInRow() * vs->GetTileInCol();
        for (uint32_t i = 0; i < tilesNum; i++)
        {
           DELETE_MEMORY(trackSegCtxs[i].initSegmenter);
           DELETE_MEMORY(trackSegCtxs[i].dashSegmenter);
        }

        delete[] trackSegCtxs;
        trackSegCtxs = NULL;
    }
    m_streamSegCtx.clear();

    std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator itExtractorCtx;
    for (itExtractorCtx = m_extractorSegCtx.begin();
        itExtractorCtx != m_extractorSegCtx.end();
        itExtractorCtx++)
    {
        TrackSegmentCtx *trackSegCtx = itExtractorCtx->second;
        if (trackSegCtx->extractorTrackNalu.data)
        {
            free(trackSegCtx->extractorTrackNalu.data);
            trackSegCtx->extractorTrackNalu.data = NULL;
        }

        DELETE_MEMORY(trackSegCtx->initSegmenter);
        DELETE_MEMORY(trackSegCtx->dashSegmenter);

        DELETE_MEMORY(trackSegCtx);
    }

    m_extractorSegCtx.clear();
    int32_t ret = pthread_mutex_destroy(&m_mutex);
    if (ret)
    {
        LOG(ERROR) << "Failed to destroy mutex of default segmentation !" << std::endl;
        return;
    }
}

int32_t ConvertRwpk(RegionWisePacking *rwpk, CodedMeta *codedMeta)
{
    if (!rwpk || !codedMeta)
        return OMAF_ERROR_NULL_PTR;

    RegionPacking regionPacking;
    regionPacking.constituentPictMatching = rwpk->constituentPicMatching;
    regionPacking.projPictureWidth = rwpk->projPicWidth;
    regionPacking.projPictureHeight = rwpk->projPicHeight;
    regionPacking.packedPictureWidth = rwpk->packedPicWidth;
    regionPacking.packedPictureHeight = rwpk->packedPicHeight;

    for (uint8_t i = 0; i < rwpk->numRegions; i++)
    {
        Region region;
        region.projTop = rwpk->rectRegionPacking[i].projRegTop;
        region.projLeft = rwpk->rectRegionPacking[i].projRegLeft;
        region.projWidth = rwpk->rectRegionPacking[i].projRegWidth;
        region.projHeight = rwpk->rectRegionPacking[i].projRegHeight;
        region.transform = rwpk->rectRegionPacking[i].transformType;
        region.packedTop = rwpk->rectRegionPacking[i].packedRegTop;
        region.packedLeft = rwpk->rectRegionPacking[i].packedRegLeft;
        region.packedWidth = rwpk->rectRegionPacking[i].packedRegWidth;
        region.packedHeight = rwpk->rectRegionPacking[i].packedRegHeight;

        regionPacking.regions.push_back(region);
    }

    codedMeta->regionPacking = regionPacking;

    return ERROR_NONE;
}

int32_t ConvertCovi(SphereRegion *spr, CodedMeta *codedMeta)
{
    if (!spr || !codedMeta)
        return OMAF_ERROR_NULL_PTR;

    Spherical sphericalCov;
    sphericalCov.cAzimuth = spr->centreAzimuth;
    sphericalCov.cElevation = spr->centreElevation;
    sphericalCov.cTilt = spr->centreTilt;
    sphericalCov.rAzimuth = spr->azimuthRange;
    sphericalCov.rElevation = spr->elevationRange;

    codedMeta->sphericalCoverage = sphericalCov;

    return ERROR_NONE;
}

int32_t FillQualityRank(CodedMeta *codedMeta, std::list<PicResolution> *picResList)
{
    if (!picResList)
        return OMAF_ERROR_NULL_PTR;

    Quality3d qualityRankCov;

    std::list<PicResolution>::iterator it;
    uint8_t qualityRankStarter = MAINSTREAM_QUALITY_RANK;
    uint8_t resNum = 0;
    for (it = picResList->begin(); it != picResList->end(); it++)
    {
        QualityInfo info;
        PicResolution picRes = *it;
        info.origWidth = picRes.width;
        info.origHeight = picRes.height;
        info.qualityRank = qualityRankStarter + resNum;
        Spherical sphere;
        sphere.cAzimuth = codedMeta->sphericalCoverage.get().cAzimuth;
        sphere.cElevation = codedMeta->sphericalCoverage.get().cElevation;
        sphere.cTilt = codedMeta->sphericalCoverage.get().cTilt;
        sphere.rAzimuth = codedMeta->sphericalCoverage.get().rAzimuth;
        sphere.rElevation = codedMeta->sphericalCoverage.get().rElevation;
        info.sphere = sphere;
        qualityRankCov.qualityInfo.push_back(info);
        resNum++;
    }
    qualityRankCov.remainingArea = true;
    codedMeta->qualityRankCoverage = qualityRankCov;

    return ERROR_NONE;
}

int32_t DefaultSegmentation::ConstructTileTrackSegCtx()
{
    std::set<uint64_t> bitRateRanking;

    std::map<uint8_t, MediaStream*>::iterator it;
    for (it = m_streamMap->begin(); it != m_streamMap->end(); it++)
    {
        MediaStream *stream = it->second;
        if (stream->GetMediaType() == VIDEOTYPE)
        {
            VideoStream *vs = (VideoStream*)stream;
            uint64_t bitRate = vs->GetBitRate();
            bitRateRanking.insert(bitRate);
        }
    }

    for (it = m_streamMap->begin(); it != m_streamMap->end(); it++)
    {
        MediaStream *stream = it->second;
        if (stream->GetMediaType() == VIDEOTYPE)
        {
            VideoStream *vs = (VideoStream*)stream;
            //TrackSegmentCtx *trackSegCtxs = vs->GetAllTrackSegCtxs();
            TileInfo *tilesInfo = vs->GetAllTilesInfo();
            Rational frameRate = vs->GetFrameRate();
            m_frameRate = frameRate;
            uint64_t bitRate = vs->GetBitRate();
            uint8_t qualityLevel = bitRateRanking.size();
            std::set<uint64_t>::iterator itBitRate;
            for (itBitRate = bitRateRanking.begin();
                itBitRate != bitRateRanking.end();
                itBitRate++, qualityLevel--)
            {
                if (*itBitRate == bitRate)
                    break;
            }
            m_projType = (VCD::OMAF::ProjectionFormat)vs->GetProjType();
            m_videoSegInfo = vs->GetVideoSegInfo();
            Nalu *vpsNalu = vs->GetVPSNalu();
            if (!vpsNalu || !(vpsNalu->data) || !(vpsNalu->dataSize))
                return OMAF_ERROR_INVALID_HEADER;

            Nalu *spsNalu = vs->GetSPSNalu();
            if (!spsNalu || !(spsNalu->data) || !(spsNalu->dataSize))
                return OMAF_ERROR_INVALID_SPS;

            Nalu *ppsNalu = vs->GetPPSNalu();
            if (!ppsNalu || !(ppsNalu->data) || !(ppsNalu->dataSize))
                return OMAF_ERROR_INVALID_PPS;

            std::vector<uint8_t> vpsData(
                static_cast<const uint8_t*>(vpsNalu->data),
                static_cast<const uint8_t*>(vpsNalu->data) + vpsNalu->dataSize);
            std::vector<uint8_t> spsData(
                static_cast<const uint8_t*>(spsNalu->data),
                static_cast<const uint8_t*>(spsNalu->data) + spsNalu->dataSize);
            std::vector<uint8_t> ppsData(
                static_cast<const uint8_t*>(ppsNalu->data),
                static_cast<const uint8_t*>(ppsNalu->data) + ppsNalu->dataSize);

            uint32_t tilesNum = vs->GetTileInRow() * vs->GetTileInCol();

            RegionWisePacking *rwpk = vs->GetSrcRwpk();

            uint64_t tileBitRate = bitRate / tilesNum;

            TrackSegmentCtx *trackSegCtxs = new TrackSegmentCtx[tilesNum];
            if (!trackSegCtxs)
                return OMAF_ERROR_NULL_PTR;
            std::map<uint32_t, TrackId> tilesTrackIndex;
            for (uint32_t i = 0; i < tilesNum; i++)
            {
                trackSegCtxs[i].isExtractorTrack = false;
                trackSegCtxs[i].tileInfo = &(tilesInfo[i]);
                trackSegCtxs[i].tileIdx = i;
                trackSegCtxs[i].trackIdx = m_trackIdStarter + i;

                //set InitSegConfig
                TrackConfig trackConfig{};
                trackConfig.meta.trackId = m_trackIdStarter + i;
                trackConfig.meta.timescale = StreamSegmenter::RatU64(frameRate.den, frameRate.num * 1000); //?
                trackConfig.meta.type = StreamSegmenter::MediaType::Video;
                trackConfig.pipelineOutput = DataInputFormat::VideoMono;
                trackSegCtxs[i].dashInitCfg.tracks.insert(std::make_pair(trackSegCtxs[i].trackIdx, trackConfig));
                m_allTileTracks.insert(std::make_pair(trackSegCtxs[i].trackIdx, trackConfig));
                trackSegCtxs[i].dashInitCfg.fragmented = true;
                trackSegCtxs[i].dashInitCfg.writeToBitstream = true;
                trackSegCtxs[i].dashInitCfg.packedSubPictures = true;
                trackSegCtxs[i].dashInitCfg.mode = OperatingMode::OMAF;
                trackSegCtxs[i].dashInitCfg.streamIds.push_back(trackConfig.meta.trackId.get());
                snprintf(trackSegCtxs[i].dashInitCfg.initSegName, 1024, "%s%s_track%ld.init.mp4", m_segInfo->dirName, m_segInfo->outName, m_trackIdStarter + i);

                //set GeneralSegConfig
                trackSegCtxs[i].dashCfg.sgtDuration = StreamSegmenter::RatU64(m_videoSegInfo->segDur, 1); //?
                trackSegCtxs[i].dashCfg.subsgtDuration = trackSegCtxs[i].dashCfg.sgtDuration / FrameDuration{ 1, 1}; //?
                trackSegCtxs[i].dashCfg.needCheckIDR = true;

                StreamSegmenter::TrackMeta trackMeta{};
                trackMeta.trackId = trackSegCtxs[i].trackIdx;
                trackMeta.timescale = StreamSegmenter::RatU64(frameRate.den, frameRate.num * 1000); //?
                trackMeta.type = StreamSegmenter::MediaType::Video;
                trackSegCtxs[i].dashCfg.tracks.insert(std::make_pair(trackSegCtxs[i].trackIdx, trackMeta));

                trackSegCtxs[i].dashCfg.useSeparatedSidx = false;
                trackSegCtxs[i].dashCfg.streamsIdx.push_back(it->first);
                snprintf(trackSegCtxs[i].dashCfg.tileSegBaseName, 1024, "%s%s_track%ld", m_segInfo->dirName, m_segInfo->outName, m_trackIdStarter + i);

                //setup DashInitSegmenter
                trackSegCtxs[i].initSegmenter = new DashInitSegmenter(&(trackSegCtxs[i].dashInitCfg));
                if (!(trackSegCtxs[i].initSegmenter))
                {
                    for (uint32_t id = 0; id < i; id++)
                    {
                        DELETE_MEMORY(trackSegCtxs[id].initSegmenter);
                        DELETE_MEMORY(trackSegCtxs[id].dashSegmenter);
                    }
                    DELETE_ARRAY(trackSegCtxs);
                    return OMAF_ERROR_NULL_PTR;
                }

                //setup DashSegmenter
                trackSegCtxs[i].dashSegmenter = new DashSegmenter(&(trackSegCtxs[i].dashCfg), true);
                if (!(trackSegCtxs[i].dashSegmenter))
                {
                    for (uint32_t id = 0; id < i; id++)
                    {
                        DELETE_MEMORY(trackSegCtxs[id].initSegmenter);
                        DELETE_MEMORY(trackSegCtxs[id].dashSegmenter);
                    }

                    DELETE_MEMORY(trackSegCtxs[i].initSegmenter);
                    DELETE_ARRAY(trackSegCtxs);
                    return OMAF_ERROR_NULL_PTR;
                }

                trackSegCtxs[i].qualityRanking = qualityLevel;

                //setup CodedMeta
                trackSegCtxs[i].codedMeta.presIndex = 0;
                trackSegCtxs[i].codedMeta.codingIndex = 0;
                trackSegCtxs[i].codedMeta.codingTime = FrameTime{ 0, 1 };
                trackSegCtxs[i].codedMeta.presTime = FrameTime{ 0, 1000 };
                trackSegCtxs[i].codedMeta.duration = FrameDuration{ frameRate.den * 1000, frameRate.num * 1000};
                trackSegCtxs[i].codedMeta.trackId = trackSegCtxs[i].trackIdx;
                trackSegCtxs[i].codedMeta.inCodingOrder = true;
                trackSegCtxs[i].codedMeta.format = CodedFormat::H265;
                trackSegCtxs[i].codedMeta.decoderConfig.insert(std::make_pair(ConfigType::VPS, vpsData));
                trackSegCtxs[i].codedMeta.decoderConfig.insert(std::make_pair(ConfigType::SPS, spsData));
                trackSegCtxs[i].codedMeta.decoderConfig.insert(std::make_pair(ConfigType::PPS, ppsData));
                trackSegCtxs[i].codedMeta.width = tilesInfo[i].tileWidth;
                trackSegCtxs[i].codedMeta.height = tilesInfo[i].tileHeight;
                trackSegCtxs[i].codedMeta.bitrate.avgBitrate = tileBitRate;
                trackSegCtxs[i].codedMeta.bitrate.maxBitrate = 0;
                trackSegCtxs[i].codedMeta.type = FrameType::IDR;
                trackSegCtxs[i].codedMeta.segmenterMeta.segmentDuration = FrameDuration{ 0, 1 }; //?


                RegionWisePacking regionPacking;
                regionPacking.constituentPicMatching = rwpk->constituentPicMatching;
                regionPacking.numRegions = 1;
                regionPacking.projPicWidth = rwpk->projPicWidth;
                regionPacking.projPicHeight = rwpk->projPicHeight;
                regionPacking.packedPicWidth = rwpk->packedPicWidth;
                regionPacking.packedPicHeight = rwpk->packedPicHeight;
                regionPacking.rectRegionPacking = new RectangularRegionWisePacking[1];
                if (!(regionPacking.rectRegionPacking))
                {
                    for (uint32_t id = 0; id < (i + 1); id++)
                    {
                        DELETE_MEMORY(trackSegCtxs[id].initSegmenter);
                        DELETE_MEMORY(trackSegCtxs[id].dashSegmenter);
                    }

                    DELETE_ARRAY(trackSegCtxs);
                    return OMAF_ERROR_NULL_PTR;
                }

                memcpy(&(regionPacking.rectRegionPacking[0]), &(rwpk->rectRegionPacking[i]), sizeof(RectangularRegionWisePacking));
                ConvertRwpk(&(regionPacking), &(trackSegCtxs[i].codedMeta));
                DELETE_ARRAY(regionPacking.rectRegionPacking);


                if (m_projType == VCD::OMAF::ProjectionFormat::PF_ERP)
                {
                    trackSegCtxs[i].codedMeta.projection = OmafProjectionType::EQUIRECTANGULAR;
                }
                else if (m_projType == VCD::OMAF::ProjectionFormat::PF_CUBEMAP)
                {
                    trackSegCtxs[i].codedMeta.projection = OmafProjectionType::CUBEMAP;
                }
                else
                {
                    for (uint32_t id = 0; id < (i + 1); id++)
                    {
                        DELETE_MEMORY(trackSegCtxs[id].initSegmenter);
                        DELETE_MEMORY(trackSegCtxs[id].dashSegmenter);
                    }

                    DELETE_ARRAY(trackSegCtxs);
                    return OMAF_ERROR_INVALID_PROJECTIONTYPE;
                }

                trackSegCtxs[i].codedMeta.isEOS = false;

                tilesTrackIndex.insert(std::make_pair(i, trackSegCtxs[i].trackIdx));

                m_trackSegCtx.insert(std::make_pair(trackSegCtxs[i].trackIdx, &(trackSegCtxs[i])));
            }
            m_trackIdStarter += tilesNum;
            m_streamSegCtx.insert(std::make_pair(stream, trackSegCtxs));
            m_framesIsKey.insert(std::make_pair(stream, true));
            m_streamsIsEOS.insert(std::make_pair(stream, false));
            m_tilesTrackIdxs.insert(std::make_pair(it->first, tilesTrackIndex));
        }
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::ConstructExtractorTrackSegCtx()
{
    std::map<uint8_t, ExtractorTrack*> *extractorTracks = m_extractorTrackMan->GetAllExtractorTracks();
    std::map<uint8_t, ExtractorTrack*>::iterator it1;
    for (it1 = extractorTracks->begin(); it1 != extractorTracks->end(); it1++)
    {
        ExtractorTrack *extractorTrack = it1->second;
        Nalu *vpsNalu = extractorTrack->GetVPS();
        Nalu *spsNalu = extractorTrack->GetSPS();
        Nalu *ppsNalu = extractorTrack->GetPPS();

        std::vector<uint8_t> vpsData(
            static_cast<const uint8_t*>(vpsNalu->data),
            static_cast<const uint8_t*>(vpsNalu->data) + vpsNalu->dataSize);
        std::vector<uint8_t> spsData(
            static_cast<const uint8_t*>(spsNalu->data),
            static_cast<const uint8_t*>(spsNalu->data) + spsNalu->dataSize);
        std::vector<uint8_t> ppsData(
            static_cast<const uint8_t*>(ppsNalu->data),
            static_cast<const uint8_t*>(ppsNalu->data) + ppsNalu->dataSize);

        RegionWisePacking *rwpk = extractorTrack->GetRwpk();
        ContentCoverage   *covi = extractorTrack->GetCovi();
        std::list<PicResolution> *picResList = extractorTrack->GetPicRes();
        Nalu *projSEI = extractorTrack->GetProjectionSEI();
        Nalu *rwpkSEI = extractorTrack->GetRwpkSEI();

        TrackSegmentCtx *trackSegCtx = new TrackSegmentCtx;
        if (!trackSegCtx)
            return OMAF_ERROR_NULL_PTR;

        trackSegCtx->isExtractorTrack = true;
        trackSegCtx->extractorTrackIdx = it1->first;
        trackSegCtx->extractors = extractorTrack->GetAllExtractors();
        memset(&(trackSegCtx->extractorTrackNalu), 0, sizeof(Nalu));
        trackSegCtx->extractorTrackNalu.dataSize = projSEI->dataSize + rwpkSEI->dataSize;
        trackSegCtx->extractorTrackNalu.data = new uint8_t[trackSegCtx->extractorTrackNalu.dataSize];
        if (!(trackSegCtx->extractorTrackNalu.data))
        {
            DELETE_MEMORY(trackSegCtx);
            return OMAF_ERROR_NULL_PTR;
        }

        memcpy(trackSegCtx->extractorTrackNalu.data, projSEI->data, projSEI->dataSize);
        memcpy(trackSegCtx->extractorTrackNalu.data + projSEI->dataSize, rwpkSEI->data, rwpkSEI->dataSize);

        TilesMergeDirectionInCol *tilesMergeDir = extractorTrack->GetTilesMergeDir();
        std::list<TilesInCol*>::iterator itCol;
        for (itCol = tilesMergeDir->tilesArrangeInCol.begin();
            itCol != tilesMergeDir->tilesArrangeInCol.end(); itCol++)
        {
            TilesInCol *tileCol = *itCol;
            std::list<SingleTile*>::iterator itTile;
            for (itTile = tileCol->begin(); itTile != tileCol->end(); itTile++)
            {
                SingleTile *tile = *itTile;
                uint8_t vsIdx    = tile->streamIdxInMedia;
                uint8_t origTileIdx  = tile->origTileIdx;

                std::map<uint8_t, std::map<uint32_t, TrackId>>::iterator itTilesIdxs;
                itTilesIdxs = m_tilesTrackIdxs.find(vsIdx);
                if (itTilesIdxs == m_tilesTrackIdxs.end())
                {
                    DELETE_ARRAY(trackSegCtx->extractorTrackNalu.data);
                    DELETE_MEMORY(trackSegCtx);
                    return OMAF_ERROR_STREAM_NOT_FOUND;
                }
                std::map<uint32_t, TrackId> tilesIndex = itTilesIdxs->second;
                TrackId foundTrackId = tilesIndex[origTileIdx];
                trackSegCtx->refTrackIdxs.push_back(foundTrackId);
            }
        }

        trackSegCtx->trackIdx = DEFAULT_EXTRACTORTRACK_TRACKIDBASE + trackSegCtx->extractorTrackIdx;

        //set up InitSegConfig
        std::set<TrackId> allTrackIds;
        std::map<TrackId, TrackConfig>::iterator itTrack;
        for (itTrack = m_allTileTracks.begin(); itTrack != m_allTileTracks.end(); itTrack++)
        {
            trackSegCtx->dashInitCfg.tracks.insert(std::make_pair(itTrack->first, itTrack->second));
            allTrackIds.insert(itTrack->first);
        }

        TrackConfig trackConfig{};
        trackConfig.meta.trackId = trackSegCtx->trackIdx;
        trackConfig.meta.timescale = StreamSegmenter::RatU64(m_frameRate.den, m_frameRate.num * 1000); //?
        trackConfig.meta.type = StreamSegmenter::MediaType::Video;
        trackConfig.trackReferences.insert(std::make_pair("scal", allTrackIds));
        trackConfig.pipelineOutput = DataInputFormat::VideoMono;
        trackSegCtx->dashInitCfg.tracks.insert(std::make_pair(trackSegCtx->trackIdx, trackConfig));

        trackSegCtx->dashInitCfg.fragmented = true;
        trackSegCtx->dashInitCfg.writeToBitstream = true;
        trackSegCtx->dashInitCfg.packedSubPictures = true;
        trackSegCtx->dashInitCfg.mode = OperatingMode::OMAF;
        trackSegCtx->dashInitCfg.streamIds.push_back(trackSegCtx->trackIdx.get());
        std::set<TrackId>::iterator itId;
        for (itId = allTrackIds.begin(); itId != allTrackIds.end(); itId++)
        {
            trackSegCtx->dashInitCfg.streamIds.push_back((*itId).get());
        }
        snprintf(trackSegCtx->dashInitCfg.initSegName, 1024, "%s%s_track%d.init.mp4", m_segInfo->dirName, m_segInfo->outName, trackSegCtx->trackIdx.get());

        //set up GeneralSegConfig
        trackSegCtx->dashCfg.sgtDuration = StreamSegmenter::RatU64(m_videoSegInfo->segDur, 1); //?
        trackSegCtx->dashCfg.subsgtDuration = trackSegCtx->dashCfg.sgtDuration / FrameDuration{ 1, 1}; //?
        trackSegCtx->dashCfg.needCheckIDR = true;

        StreamSegmenter::TrackMeta trackMeta{};
        trackMeta.trackId = trackSegCtx->trackIdx;
        trackMeta.timescale = StreamSegmenter::RatU64(m_frameRate.den, m_frameRate.num * 1000); //?
        trackMeta.type = StreamSegmenter::MediaType::Video;
        trackSegCtx->dashCfg.tracks.insert(std::make_pair(trackSegCtx->trackIdx, trackMeta));

        trackSegCtx->dashCfg.useSeparatedSidx = false;
        trackSegCtx->dashCfg.streamsIdx.push_back(trackSegCtx->trackIdx.get());
        snprintf(trackSegCtx->dashCfg.tileSegBaseName, 1024, "%s%s_track%d", m_segInfo->dirName, m_segInfo->outName, trackSegCtx->trackIdx.get());

        //set up DashInitSegmenter
        trackSegCtx->initSegmenter = new DashInitSegmenter(&(trackSegCtx->dashInitCfg));
        if (!(trackSegCtx->initSegmenter))
        {
            DELETE_ARRAY(trackSegCtx->extractorTrackNalu.data);
            DELETE_MEMORY(trackSegCtx);
            return OMAF_ERROR_NULL_PTR;
        }

        //set up DashSegmenter
        trackSegCtx->dashSegmenter = new DashSegmenter(&(trackSegCtx->dashCfg), true);
        if (!(trackSegCtx->dashSegmenter))
        {
            DELETE_ARRAY(trackSegCtx->extractorTrackNalu.data);
            DELETE_MEMORY(trackSegCtx->initSegmenter);
            DELETE_MEMORY(trackSegCtx);
            return OMAF_ERROR_NULL_PTR;
        }

        //set up CodedMeta
        trackSegCtx->codedMeta.presIndex = 0;
        trackSegCtx->codedMeta.codingIndex = 0;
        trackSegCtx->codedMeta.codingTime = FrameTime{ 0, 1 };
        trackSegCtx->codedMeta.presTime = FrameTime{ 0, 1000 };
        trackSegCtx->codedMeta.duration = FrameDuration{ m_frameRate.den * 1000, m_frameRate.num * 1000};
        trackSegCtx->codedMeta.trackId = trackSegCtx->trackIdx;
        trackSegCtx->codedMeta.inCodingOrder = true;
        trackSegCtx->codedMeta.format = CodedFormat::H265Extractor;
        trackSegCtx->codedMeta.decoderConfig.insert(std::make_pair(ConfigType::VPS, vpsData));
        trackSegCtx->codedMeta.decoderConfig.insert(std::make_pair(ConfigType::SPS, spsData));
        trackSegCtx->codedMeta.decoderConfig.insert(std::make_pair(ConfigType::PPS, ppsData));
        trackSegCtx->codedMeta.width = rwpk->packedPicWidth;//tilesInfo[i].tileWidth;
        trackSegCtx->codedMeta.height = rwpk->packedPicHeight;//tilesInfo[i].tileHeight;
        trackSegCtx->codedMeta.bitrate.avgBitrate = 0;
        trackSegCtx->codedMeta.bitrate.maxBitrate = 0;
        trackSegCtx->codedMeta.type = FrameType::IDR;
        trackSegCtx->codedMeta.segmenterMeta.segmentDuration = FrameDuration{ 0, 1 }; //?
        ConvertRwpk(rwpk, &(trackSegCtx->codedMeta));
        ConvertCovi(covi->sphereRegions, &(trackSegCtx->codedMeta));

        FillQualityRank(&(trackSegCtx->codedMeta), picResList);

        if (m_projType == VCD::OMAF::ProjectionFormat::PF_ERP)
        {
            trackSegCtx->codedMeta.projection = OmafProjectionType::EQUIRECTANGULAR;
        }
        else if (m_projType == VCD::OMAF::ProjectionFormat::PF_CUBEMAP)
        {
            trackSegCtx->codedMeta.projection = OmafProjectionType::CUBEMAP;
        }
        else
        {
            DELETE_ARRAY(trackSegCtx->extractorTrackNalu.data);
            DELETE_MEMORY(trackSegCtx->initSegmenter);
            DELETE_MEMORY(trackSegCtx->dashSegmenter);
            DELETE_MEMORY(trackSegCtx);
            return OMAF_ERROR_INVALID_PROJECTIONTYPE;
        }

        trackSegCtx->codedMeta.isEOS = false;

        m_extractorSegCtx.insert(std::make_pair(extractorTrack, trackSegCtx));
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::VideoEndSegmentation()
{
    std::map<uint8_t, MediaStream*>::iterator it = m_streamMap->begin();
    for ( ; it != m_streamMap->end(); it++)
    {
        MediaStream *stream = it->second;
        if (stream->GetMediaType() == VIDEOTYPE)
        {
            int32_t ret = EndEachVideo(stream);
            if (ret)
                return ret;
        }
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::WriteSegmentForEachVideo(MediaStream *stream, bool isKeyFrame, bool isEOS)
{
    if (!stream)
        return OMAF_ERROR_NULL_PTR;

    VideoStream *vs = (VideoStream*)stream;

    std::map<MediaStream*, TrackSegmentCtx*>::iterator itStreamTrack;
    itStreamTrack = m_streamSegCtx.find(stream);
    if (itStreamTrack == m_streamSegCtx.end())
        return OMAF_ERROR_STREAM_NOT_FOUND;

    TrackSegmentCtx *trackSegCtxs = itStreamTrack->second;

    uint32_t tilesNum = vs->GetTileInRow() * vs->GetTileInCol();
    for (uint32_t tileIdx = 0; tileIdx < tilesNum; tileIdx++)
    {
        DashSegmenter *dashSegmenter = trackSegCtxs[tileIdx].dashSegmenter;
        if (!dashSegmenter)
            return OMAF_ERROR_NULL_PTR;

        if (isKeyFrame)
            trackSegCtxs[tileIdx].codedMeta.type = FrameType::IDR;
        else
            trackSegCtxs[tileIdx].codedMeta.type = FrameType::NONIDR;

        trackSegCtxs[tileIdx].codedMeta.isEOS = isEOS;

        int32_t ret = dashSegmenter->SegmentData(&(trackSegCtxs[tileIdx]));
        if (ret)
            return ret;

        trackSegCtxs[tileIdx].codedMeta.presIndex++;
        trackSegCtxs[tileIdx].codedMeta.codingIndex++;
        trackSegCtxs[tileIdx].codedMeta.presTime.num += 1000 / (m_frameRate.num / m_frameRate.den);
        trackSegCtxs[tileIdx].codedMeta.presTime.den = 1000;

        m_segNum = dashSegmenter->GetSegmentsNum();
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::WriteSegmentForEachExtractorTrack(
    ExtractorTrack *extractorTrack,
    bool isKeyFrame,
    bool isEOS)
{
    if (!extractorTrack)
        return OMAF_ERROR_NULL_PTR;

    std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator it;
    it = m_extractorSegCtx.find(extractorTrack);
    if (it == m_extractorSegCtx.end())
        return OMAF_ERROR_EXTRACTORTRACK_NOT_FOUND;

    TrackSegmentCtx *trackSegCtx = it->second;
    if (!trackSegCtx)
        return OMAF_ERROR_NULL_PTR;

    if (isKeyFrame)
        trackSegCtx->codedMeta.type = FrameType::IDR;
    else
        trackSegCtx->codedMeta.type = FrameType::NONIDR;

    trackSegCtx->codedMeta.isEOS = isEOS;

    DashSegmenter *dashSegmenter = trackSegCtx->dashSegmenter;
    if (!dashSegmenter)
       return OMAF_ERROR_NULL_PTR;

    int32_t ret = dashSegmenter->SegmentData(trackSegCtx);
    if (ret)
        return ret;

    trackSegCtx->codedMeta.presIndex++;
    trackSegCtx->codedMeta.codingIndex++;
    trackSegCtx->codedMeta.presTime.num += 1000 / (m_frameRate.num / m_frameRate.den);
    trackSegCtx->codedMeta.presTime.den = 1000;

    return ERROR_NONE;
}

int32_t DefaultSegmentation::StartExtractorTrackSegmentation(
    ExtractorTrack *extractorTrack)
{
    pthread_t threadId;
    int32_t ret = pthread_create(&threadId, NULL, ExtractorTrackSegThread, this);

    if (ret)
    {
        LOG(ERROR) << "Failed to create extractor track segmentation thread !" << std::endl;
        return OMAF_ERROR_CREATE_THREAD;
    }

    m_extractorThreadIds.insert(std::make_pair(threadId, extractorTrack));
    return ERROR_NONE;
}

int32_t DefaultSegmentation::StartLastExtractorTrackSegmentation(
    ExtractorTrack *extractorTrack)
{
    pthread_t threadId;
    int32_t ret = pthread_create(&threadId, NULL, LastExtractorTrackSegThread, this);

    if (ret)
    {
        LOG(ERROR) << "Failed to create extractor track segmentation thread !" << std::endl;
        return OMAF_ERROR_CREATE_THREAD;
    }

    m_extractorThreadIds.insert(std::make_pair(threadId, extractorTrack));
    return ERROR_NONE;
}

void *DefaultSegmentation::ExtractorTrackSegThread(void *pThis)
{
    DefaultSegmentation *defaultSegmentation = (DefaultSegmentation*)pThis;

    defaultSegmentation->ExtractorTrackSegmentation();

    return NULL;
}

void *DefaultSegmentation::LastExtractorTrackSegThread(void *pThis)
{
    DefaultSegmentation *defaultSegmentation = (DefaultSegmentation*)pThis;

    defaultSegmentation->LastExtractorTrackSegmentation();

    return NULL;
}

int32_t DefaultSegmentation::ExtractorTrackSegmentation()
{
    while(1)
    {

        std::map<uint8_t, ExtractorTrack*> *extractorTracks = m_extractorTrackMan->GetAllExtractorTracks();
        std::map<uint8_t, ExtractorTrack*>::iterator itExtractorTrack;

        pthread_t threadId = pthread_self();
        ExtractorTrack *extractorTrack = m_extractorThreadIds[threadId];
        if (!extractorTrack)
            return OMAF_ERROR_NULL_PTR;

        for (itExtractorTrack = extractorTracks->begin();
            itExtractorTrack != extractorTracks->end(); itExtractorTrack++)
        {
            if (itExtractorTrack->second == extractorTrack)
                break;
        }
        if (itExtractorTrack == extractorTracks->end())
        {
            LOG(ERROR) << "Can't find specified Extractor Track! " << std::endl;
            return OMAF_ERROR_INVALID_DATA;
        }
        while (!(extractorTrack->GetFramesReadyStatus()))
        {
            usleep(50);
        }

        uint8_t etId = 0;
        for ( ; etId < m_aveETPerSegThread; etId++)
        {

            if (itExtractorTrack == extractorTracks->end())
            {
                LOG(ERROR) << "Can't find specified Extractor Track! " << std::endl;
                return OMAF_ERROR_INVALID_DATA;
            }

            ExtractorTrack *extractorTrack1 = itExtractorTrack->second;

            extractorTrack1->ConstructExtractors();
            WriteSegmentForEachExtractorTrack(extractorTrack1, m_nowKeyFrame, m_isEOS);

            std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator itET;
            itET = m_extractorSegCtx.find(extractorTrack1);
            if (itET == m_extractorSegCtx.end())
            {
                LOG(ERROR) << "Can't find segmentation context for specified extractor track !" << std::endl;
                return OMAF_ERROR_INVALID_DATA;
            }
            TrackSegmentCtx *trackSegCtx = itET->second;

            if (m_segNum == (m_prevSegNum + 1))
            {
                extractorTrack1->DestroyCurrSegNalus();
            }

            if (trackSegCtx->extractorTrackNalu.data)
            {
                extractorTrack1->AddExtractorsNaluToSeg(trackSegCtx->extractorTrackNalu.data);
                trackSegCtx->extractorTrackNalu.data = NULL;
            }
            trackSegCtx->extractorTrackNalu.dataSize = 0;

            extractorTrack1->IncreaseProcessedFrmNum();
            itExtractorTrack++;
        }
        if (m_isEOS)
            //return ERROR_NONE;
            break;
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::LastExtractorTrackSegmentation()
{
    while(1)
    {
        std::map<uint8_t, ExtractorTrack*> *extractorTracks = m_extractorTrackMan->GetAllExtractorTracks();
        std::map<uint8_t, ExtractorTrack*>::iterator itExtractorTrack;

        pthread_t threadId = pthread_self();
        ExtractorTrack *extractorTrack = m_extractorThreadIds[threadId];
        if (!extractorTrack)
            return OMAF_ERROR_NULL_PTR;

        for (itExtractorTrack = extractorTracks->begin();
            itExtractorTrack != extractorTracks->end(); itExtractorTrack++)
        {
            if (itExtractorTrack->second == extractorTrack)
                break;
        }
        if (itExtractorTrack == extractorTracks->end())
        {
            LOG(ERROR) << "Can't find specified Extractor Track! " << std::endl;
            return OMAF_ERROR_INVALID_DATA;
        }

        while (!(extractorTrack->GetFramesReadyStatus()))
        {
            usleep(50);
        }

        uint8_t etId = 0;
        for ( ; etId < m_lastETPerSegThread; etId++)
        {

            if (itExtractorTrack == extractorTracks->end())
            {
                LOG(ERROR) << "Can't find specified Extractor Track! " << std::endl;
                return OMAF_ERROR_INVALID_DATA;
            }

            ExtractorTrack *extractorTrack1 = itExtractorTrack->second;

            extractorTrack1->ConstructExtractors();
            WriteSegmentForEachExtractorTrack(extractorTrack1, m_nowKeyFrame, m_isEOS);

            std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator itET;
            itET = m_extractorSegCtx.find(extractorTrack1);
            if (itET == m_extractorSegCtx.end())
            {
                LOG(ERROR) << "Can't find segmentation context for specified extractor track !" << std::endl;
                return OMAF_ERROR_INVALID_DATA;
            }
            TrackSegmentCtx *trackSegCtx = itET->second;

            if (m_segNum == (m_prevSegNum + 1))
            {
                extractorTrack1->DestroyCurrSegNalus();
            }

            if (trackSegCtx->extractorTrackNalu.data)
            {
                extractorTrack1->AddExtractorsNaluToSeg(trackSegCtx->extractorTrackNalu.data);
                trackSegCtx->extractorTrackNalu.data = NULL;
            }
            trackSegCtx->extractorTrackNalu.dataSize = 0;

            extractorTrack1->IncreaseProcessedFrmNum();
            itExtractorTrack++;
        }
        if (m_isEOS)
            //return ERROR_NONE;
            break;
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::VideoSegmentation()
{
    uint64_t currentT = 0;
    int32_t ret = ConstructTileTrackSegCtx();
    if (ret)
        return ret;

    ret = ConstructExtractorTrackSegCtx();
    if (ret)
        return ret;

    m_mpdGen = new MpdGenerator(
                    &m_streamSegCtx,
                    &m_extractorSegCtx,
                    m_segInfo,
                    m_projType,
                    m_frameRate);
    if (!m_mpdGen)
        return OMAF_ERROR_NULL_PTR;

    ret = m_mpdGen->Initialize();

    if (ret)
        return ret;


    std::map<MediaStream*, TrackSegmentCtx*>::iterator itStreamTrack;
    for (itStreamTrack = m_streamSegCtx.begin(); itStreamTrack != m_streamSegCtx.end(); itStreamTrack++)
    {
        MediaStream *stream = itStreamTrack->first;
        TrackSegmentCtx* trackSegCtxs = itStreamTrack->second;

        if (stream->GetMediaType() == VIDEOTYPE)
        {
            VideoStream *vs = (VideoStream*)stream;
            uint32_t tilesNum = vs->GetTileInRow() * vs->GetTileInCol();
            for (uint32_t tileIdx = 0; tileIdx < tilesNum; tileIdx++)
            {
                DashInitSegmenter *initSegmenter = trackSegCtxs[tileIdx].initSegmenter;
                if (!initSegmenter)
                    return OMAF_ERROR_NULL_PTR;

                ret = initSegmenter->GenerateInitSegment(&(trackSegCtxs[tileIdx]), m_trackSegCtx);
                if (ret)
                    return ret;

            }
        }
    }

    std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator itExtractorTrack;
    for (itExtractorTrack = m_extractorSegCtx.begin();
        itExtractorTrack != m_extractorSegCtx.end();
        itExtractorTrack++)
    {
        TrackSegmentCtx *trackSegCtx =  itExtractorTrack->second;

        DashInitSegmenter *initSegmenter = trackSegCtx->initSegmenter;
        if (!initSegmenter)
            return OMAF_ERROR_NULL_PTR;

        ret = initSegmenter->GenerateInitSegment(trackSegCtx, m_trackSegCtx);
        if (ret)
            return ret;
    }

    m_prevSegNum = m_segNum;

    uint16_t extractorTrackNum = m_extractorSegCtx.size();
    if (extractorTrackNum % m_segInfo->extractorTracksPerSegThread == 0)
    {
        m_aveETPerSegThread = m_segInfo->extractorTracksPerSegThread;
        m_lastETPerSegThread = m_segInfo->extractorTracksPerSegThread;
        m_threadNumForET = extractorTrackNum / m_segInfo->extractorTracksPerSegThread;
    }
    else
    {
        m_aveETPerSegThread = m_segInfo->extractorTracksPerSegThread;
        m_lastETPerSegThread = extractorTrackNum % m_segInfo->extractorTracksPerSegThread;
        m_threadNumForET = extractorTrackNum / m_segInfo->extractorTracksPerSegThread + 1;
    }

    LOG(INFO) << "Lanuch  " << m_threadNumForET << " threads for Extractor Track segmentation!" << std::endl;
    LOG(INFO) << "Average Extractor Track number per thread is  " << m_aveETPerSegThread << std::endl;
    LOG(INFO) << "The last thread involves  " << m_lastETPerSegThread << " Extractor Tracks !" << std::endl;

    while (1)
    {
        if (m_segNum == 1)
        {
            if (m_segInfo->isLive)
            {
                m_mpdGen->UpdateMpd(m_segNum, m_framesNum);
            }
        }

        std::map<uint8_t, MediaStream*>::iterator itStream = m_streamMap->begin();
        for ( ; itStream != m_streamMap->end(); itStream++)
        {
            MediaStream *stream = itStream->second;
            if (stream->GetMediaType() == VIDEOTYPE)
            {
                VideoStream *vs = (VideoStream*)stream;
                vs->SetCurrFrameInfo();
                FrameBSInfo *currFrame = vs->GetCurrFrameInfo();

                while (!currFrame)
                {
                    usleep(50);
                    vs->SetCurrFrameInfo();
                    currFrame = vs->GetCurrFrameInfo();
                    if (!currFrame && (vs->GetEOS()))
                        break;
                }

                if (currFrame)
                {
                    m_framesIsKey[vs] = currFrame->isKeyFrame;
                    m_streamsIsEOS[vs] = false;

                    vs->UpdateTilesNalu();
                    WriteSegmentForEachVideo(vs, currFrame->isKeyFrame, false);
                }
                else
                {
                    m_framesIsKey[vs] = false;
                    m_streamsIsEOS[vs] = true;

                    WriteSegmentForEachVideo(vs, false, true);
                }
            }
        }

        std::map<MediaStream*, bool>::iterator itKeyFrame = m_framesIsKey.begin();
        if (itKeyFrame == m_framesIsKey.end())
            return OMAF_ERROR_INVALID_DATA;
        bool frameIsKey = itKeyFrame->second;
        itKeyFrame++;
        for ( ; itKeyFrame != m_framesIsKey.end(); itKeyFrame++)
        {
            bool keyFrame = itKeyFrame->second;
            if (frameIsKey != keyFrame)
                return OMAF_ERROR_INVALID_DATA;

        }
        m_nowKeyFrame = frameIsKey;

        std::map<MediaStream*, bool>::iterator itEOS = m_streamsIsEOS.begin();
        if (itEOS == m_streamsIsEOS.end())
            return OMAF_ERROR_STREAM_NOT_FOUND;
        bool nowEOS = itEOS->second;
        itEOS++;
        for ( ; itEOS != m_streamsIsEOS.end(); itEOS++)
        {
            bool eos = itEOS->second;
            if (nowEOS != eos)
                return OMAF_ERROR_INVALID_DATA;
        }
        m_isEOS = nowEOS;

        std::map<uint8_t, ExtractorTrack*> *extractorTracks = m_extractorTrackMan->GetAllExtractorTracks();
        std::map<uint8_t, ExtractorTrack*>::iterator itExtractorTrack = extractorTracks->begin();
        for ( ; itExtractorTrack != extractorTracks->end(); /*itExtractorTrack++*/)
        {
            ExtractorTrack *extractorTrack = itExtractorTrack->second;
            extractorTrack->SetFramesReady(true);
            if (m_extractorThreadIds.size() < m_threadNumForET)
            {
                if (m_aveETPerSegThread == m_lastETPerSegThread)
                {
                    int32_t retET = StartExtractorTrackSegmentation(extractorTrack);
                    if (retET)
                        return retET;

                    for (uint16_t num = 0; num < m_aveETPerSegThread; num++)
                    {
                        itExtractorTrack++;
                    }
                }
                else
                {
                    if ((uint16_t)(m_extractorThreadIds.size()) < (m_threadNumForET - 1))
                    {
                        int32_t retET = StartExtractorTrackSegmentation(extractorTrack);
                        if (retET)
                            return retET;

                        for (uint16_t num = 0; num < m_aveETPerSegThread; num++)
                        {
                            itExtractorTrack++;
                        }
                    }
                    else
                    {
                        int32_t retET = StartLastExtractorTrackSegmentation(extractorTrack);
                        if (retET)
                            return retET;

                        for ( ; itExtractorTrack != extractorTracks->end(); )
                        {
                            itExtractorTrack++;
                        }
                    }
                }
            }
            else
            {
                itExtractorTrack++;
            }
        }
        if (m_extractorThreadIds.size() != m_threadNumForET)
        {
            LOG(ERROR) << "Launched threads number  " << (m_extractorThreadIds.size()) << " doesn't match calculated threads number  " << m_threadNumForET << std::endl;
        }

        usleep(2000);

        for (itExtractorTrack = extractorTracks->begin();
            itExtractorTrack != extractorTracks->end();
            itExtractorTrack++)
        {
            ExtractorTrack *extractorTrack = itExtractorTrack->second;
            while (extractorTrack->GetProcessedFrmNum() == m_framesNum)
            {
                usleep(1);

                if (extractorTrack->GetProcessedFrmNum() == (m_framesNum + 1))
                    break;
            }
        }

        for (itStream = m_streamMap->begin(); itStream != m_streamMap->end(); itStream++)
        {
            MediaStream *stream = itStream->second;
            if (stream->GetMediaType() == VIDEOTYPE)
            {
                VideoStream *vs = (VideoStream*)stream;

                if (m_segNum == (m_prevSegNum + 1))
                {
                    vs->DestroyCurrSegmentFrames();
                }

                vs->AddFrameToSegment();
            }
        }
        //m_framesNum++;

        if (m_segNum == (m_prevSegNum + 1))
        {
            m_prevSegNum++;

            std::chrono::high_resolution_clock clock;
            uint64_t before = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
            LOG(INFO) << "Complete one seg on  " << (before - currentT) << " ms" << std::endl;
            currentT = before;
        }

        if (m_segInfo->isLive)
        {
            if (m_segInfo->windowSize && m_segInfo->extraWindowSize)
            {
                int32_t removeCnt = m_segNum - m_segInfo->windowSize - m_segInfo->extraWindowSize;
                if (removeCnt > 0)
                {
                    std::map<TrackId, TrackConfig>::iterator itOneTrack;
                    for (itOneTrack = m_allTileTracks.begin();
                        itOneTrack != m_allTileTracks.end();
                        itOneTrack++)
                    {
                        TrackId trackIndex = itOneTrack->first;
                        char rmFile[1024];
                        snprintf(rmFile, 1024, "%s%s_track%d.%d.mp4", m_segInfo->dirName, m_segInfo->outName, trackIndex.get(), removeCnt);
                        remove(rmFile);
                    }
                    std::map<ExtractorTrack*, TrackSegmentCtx*>::iterator itOneExtractorTrack;
                    for (itOneExtractorTrack = m_extractorSegCtx.begin();
                        itOneExtractorTrack != m_extractorSegCtx.end();
                        itOneExtractorTrack++)
                    {
                        TrackSegmentCtx *trackSegCtx = itOneExtractorTrack->second;
                        TrackId trackIndex = trackSegCtx->trackIdx;
                        char rmFile[1024];
                        snprintf(rmFile, 1024, "%s%s_track%d.%d.mp4", m_segInfo->dirName, m_segInfo->outName, trackIndex.get(), removeCnt);
                        remove(rmFile);
                    }
                }
            }
        }

        if (m_isEOS)
        {
            if (m_segInfo->isLive)
            {
                int32_t ret = m_mpdGen->UpdateMpd(m_segNum, m_framesNum);
                if (ret)
                    return ret;
            } else {
                int32_t ret = m_mpdGen->WriteMpd(m_framesNum);
                if (ret)
                    return ret;
            }
            LOG(INFO) << "Total  " << m_framesNum << " frames written into segments!" << std::endl;
            //return ERROR_NONE;
            break;
        }
        m_framesNum++;
    }

    return ERROR_NONE;
}

int32_t DefaultSegmentation::EndEachVideo(MediaStream *stream)
{
    if (!stream)
        return OMAF_ERROR_NULL_PTR;

    VideoStream *vs = (VideoStream*)stream;
    vs->SetEOS(true);

    return ERROR_NONE;
}

VCD_NS_END
