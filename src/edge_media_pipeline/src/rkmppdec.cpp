#include "rkmppdec.h"
#include "mpp_common_utils.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include "linux_dma_heap_compat.h"
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace
{
constexpr MppPollType kPollTimeout500Ms   = static_cast<MppPollType>(500);
constexpr auto        kHolderWaitInterval = std::chrono::milliseconds(100);
constexpr auto        kDecodeRetrySleep   = std::chrono::milliseconds(2);
constexpr int         kDecodePutRetryCnt  = 8;
constexpr int         kDecodeGetRetryCnt  = 8;

bool dma_buf_sync_end_read(int fd)
{
    if (fd < 0)
        return false;
    dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
}
}  // namespace

MppDecInstance::MppDecInstance()
{
    for (auto& fd_info : MppOutputFDList_)
    {
        fd_info.fd         = -1;
        fd_info.base       = nullptr;
        fd_info.size       = 0;
        fd_info.format     = 0;
        fd_info.width      = 0;
        fd_info.height     = 0;
        fd_info.hor_stride = 0;
        fd_info.ver_stride = 0;
        fd_info.pts_us     = -1;
        fd_info.dts_us     = -1;
    }

    for (auto& holder : HolderPool_)
    {
        FreeHolderQueue_.push_back(&holder);
    }
}

MppDecInstance::~MppDecInstance()
{
    ForceRecycleAllHolders();
    DrainPendingRecycleQueue();

    for (auto& buffer : MppBuffers)
    {
        if (buffer)
        {
            mpp_buffer_put(buffer);
            buffer = nullptr;
        }
    }

    if (group_)
    {
        (void)mpp_buffer_group_clear(group_);
        ReleaseExternalOutputBuffers();
        mpp_buffer_group_put(group_);
        group_ = nullptr;
    }
    if (packet_group_)
    {
        (void)mpp_buffer_group_clear(packet_group_);
        mpp_buffer_group_put(packet_group_);
        packet_group_ = nullptr;
    }

    if (mpp_ctx_)
    {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = nullptr;
        mpp_api_ = nullptr;
    }
}

MppDecInstance::DecodedTaskHolder* MppDecInstance::AcquireDecodedTaskHolder()
{
    std::lock_guard<std::mutex> lock(HolderMutex_);
    if (FreeHolderQueue_.empty())
        return nullptr;
    DecodedTaskHolder* holder = FreeHolderQueue_.front();
    FreeHolderQueue_.pop_front();
    return holder;
}

void MppDecInstance::ReturnDecodedTaskHolder(DecodedTaskHolder* holder)
{
    if (!holder)
        return;
    holder->output_desc  = nullptr;
    holder->output_frame = nullptr;
    holder->output_buf   = nullptr;
    std::lock_guard<std::mutex> lock(HolderMutex_);
    FreeHolderQueue_.push_back(holder);
}

void MppDecInstance::ReleaseTaskPacket(MppTask task)
{
    if (!task)
        return;
    MppPacket packet_from_task = nullptr;
    if (mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packet_from_task) == MPP_OK &&
        packet_from_task)
    {
        mpp_packet_deinit(&packet_from_task);
    }
}

void MppDecInstance::RecycleDecodedTaskHolder(DecodedTaskHolder* holder)
{
    if (!holder)
        return;
    if (holder->output_frame)
    {
        mpp_frame_deinit(&holder->output_frame);
        holder->output_frame = nullptr;
    }
    if (holder->output_buf)
    {
        mpp_buffer_put(holder->output_buf);
        holder->output_buf = nullptr;
    }
    ReturnDecodedTaskHolder(holder);
}

void MppDecInstance::DrainPendingRecycleQueue()
{
    std::deque<DecodedTaskHolder*> local_queue;
    {
        std::lock_guard<std::mutex> lock(HolderMutex_);
        local_queue.swap(PendingRecycleQueue_);
    }
    while (!local_queue.empty())
    {
        DecodedTaskHolder* holder = local_queue.front();
        local_queue.pop_front();
        RecycleDecodedTaskHolder(holder);
    }
}

void MppDecInstance::ForceRecycleAllHolders()
{
    std::deque<DecodedTaskHolder*> local_queue;
    {
        std::lock_guard<std::mutex> lock(HolderMutex_);
        for (const auto& item : OutDesc2HolderMap_)
        {
            local_queue.push_back(item.second);
        }
        OutDesc2HolderMap_.clear();
        while (!PendingRecycleQueue_.empty())
        {
            local_queue.push_back(PendingRecycleQueue_.front());
            PendingRecycleQueue_.pop_front();
        }
    }
    while (!local_queue.empty())
    {
        DecodedTaskHolder* holder = local_queue.front();
        local_queue.pop_front();
        RecycleDecodedTaskHolder(holder);
    }
}

int MppDecInstance::DecQueueOutputForRecycle(const IO_FD_t* output_desc)
{
    if (!output_desc)
        return -1;
    std::lock_guard<std::mutex> lock(HolderMutex_);
    const auto                  it = OutDesc2HolderMap_.find(output_desc);
    if (it == OutDesc2HolderMap_.end())
        return -1;
    PendingRecycleQueue_.push_back(it->second);
    OutDesc2HolderMap_.erase(it);
    return 0;
}

int MppDecInstance::DecInit(MppCodingType coding_type)
{
    if (mpp_ctx_ && mpp_api_)
    {
        return (coding_type_ == coding_type) ? 0 : -1;
    }
    coding_type_ = coding_type;

    MPP_RET   ret           = MPP_NOK;
    MppDecCfg dec_cfg       = nullptr;
    RK_U32    output_format = MPP_FMT_YUV420SP;
    RK_U32    split_parse   = (coding_type_ == MPP_VIDEO_CodingMJPEG) ? 0 : 1;

    ret = mpp_create(&mpp_ctx_, &mpp_api_);
    if (ret != MPP_OK || !mpp_ctx_ || !mpp_api_)
        goto fail;

    ret = mpp_init(mpp_ctx_, MPP_CTX_DEC, coding_type_);
    if (ret != MPP_OK)
        goto fail;

    ret = mpp_dec_cfg_init(&dec_cfg);
    if (ret != MPP_OK || !dec_cfg)
        goto fail;

    ret = mpp_api_->control(mpp_ctx_, MPP_DEC_GET_CFG, dec_cfg);
    if (ret == MPP_OK)
        ret = mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", split_parse);
    if (ret == MPP_OK)
        ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_CFG, dec_cfg);

    mpp_dec_cfg_deinit(dec_cfg);
    dec_cfg = nullptr;
    if (ret != MPP_OK)
        goto fail;

    ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
    if (ret != MPP_OK)
        goto fail;

    return 0;

fail:
    if (dec_cfg)
        mpp_dec_cfg_deinit(dec_cfg);
    if (mpp_ctx_)
        mpp_destroy(mpp_ctx_);
    mpp_ctx_ = nullptr;
    mpp_api_ = nullptr;
    return -1;
}

int MppDecInstance::DecAllocBuffer(const FrameDesc* frame_desc_array, size_t frame_desc_count)
{
    if (!mpp_ctx_ || !mpp_api_ || OutSize_ == 0)
        return -1;
    if (group_)
        return 0;

    // 【核心解药】：H.264/HEVC 彻底跳过外部缓冲池绑定！让 MPP 内部自动无缝管理内存！
    if (coding_type_ != MPP_VIDEO_CodingMJPEG)
    {
        return 0;
    }

    MPP_RET ret = mpp_buffer_group_get_external(&group_, MPP_BUFFER_TYPE_EXT_DMA);
    if (ret != MPP_OK || !group_)
        return -1;

    ret = mpp_buffer_group_limit_config(group_, OutSize_, resource_limits::kMppOutputBufferCount);
    if (ret != MPP_OK)
        return -1;

    if (CommitExternalOutputBuffers(OutSize_) != 0)
        return -1;

    ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_EXT_BUF_GROUP, group_);
    if (ret != MPP_OK)
        return -1;

    if (!frame_desc_array || frame_desc_count == 0)
        return 0;

    size_t import_limit = (frame_desc_count < resource_limits::kMppImportBufferCount)
                              ? frame_desc_count
                              : resource_limits::kMppImportBufferCount;
    for (size_t i = 0; i < import_limit; ++i)
    {
        const FrameDesc& frame_desc = frame_desc_array[i];
        if (frame_desc.fd < 0 || frame_desc.Length == 0)
            continue;
        const int     slot = frame_desc.index;
        MppBufferInfo info{};
        info.type  = MPP_BUFFER_TYPE_EXT_DMA;
        info.fd    = frame_desc.fd;
        info.size  = frame_desc.Length;
        info.index = slot;
        mpp_buffer_import(&MppBuffers[slot], &info);
    }
    return 0;
}

int MppDecInstance::DecConfigWidthHeight(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return -1;
    ImgWidth_  = width;
    ImgHeight_ = height;
    H_Stride_  = mpp_common::Align16(ImgWidth_);
    V_Stride_  = mpp_common::Align16(ImgHeight_);
    OutSize_   = static_cast<size_t>(H_Stride_) * static_cast<size_t>(V_Stride_) * 2u;
    return 0;
}

int MppDecInstance::ResetDecoder()
{
    if (coding_type_ == MPP_VIDEO_CodingMJPEG)
    {
        return 0;
    }

    const MppCodingType coding_type = coding_type_;
    const uint32_t      width       = ImgWidth_;
    const uint32_t      height      = ImgHeight_;

    ForceRecycleAllHolders();
    DrainPendingRecycleQueue();
    CurrentOutputDesc = nullptr;

    if (group_)
    {
        (void)mpp_buffer_group_clear(group_);
        ReleaseExternalOutputBuffers();
        mpp_buffer_group_put(group_);
        group_ = nullptr;
    }
    if (packet_group_)
    {
        (void)mpp_buffer_group_clear(packet_group_);
        mpp_buffer_group_put(packet_group_);
        packet_group_ = nullptr;
    }
    if (mpp_ctx_)
    {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = nullptr;
        mpp_api_ = nullptr;
    }

    if (DecInit(coding_type) != 0)
        return -1;
    if (DecConfigWidthHeight(width, height) != 0)
        return -1;
    if (DecAllocBuffer(nullptr, 0) != 0)
        return -1;
    return 0;
}
int MppDecInstance::MppDecode(const FrameDesc* frame_desc)
{
    DrainPendingRecycleQueue();
    CurrentOutputDesc = nullptr;

    if (!frame_desc || !mpp_ctx_ || !mpp_api_ || OutSize_ == 0)
        return -1;
    if (!frame_desc->base || frame_desc->payloadSize == 0)
        return -1;

    FrameDesc*  mutable_frame_desc = const_cast<FrameDesc*>(frame_desc);
    const auto* input_data         = static_cast<const uint8_t*>(frame_desc->base);
    const bool  is_mjpeg           = (coding_type_ == MPP_VIDEO_CodingMJPEG);
    size_t      effective_size =
        is_mjpeg ? mpp_common::FindJpegEffectiveSize(input_data, frame_desc->payloadSize)
                 : frame_desc->payloadSize;

    if (effective_size == 0)
        return -1;

    if (mutable_frame_desc->cpuSyncReadActive)
    {
        dma_buf_sync_end_read(mutable_frame_desc->fd);
        mutable_frame_desc->cpuSyncReadActive = false;
    }

    int64_t packet_pts_us = frame_desc->pts_us;
    int64_t packet_dts_us = frame_desc->dts_us;
    if (packet_pts_us >= 0)
        packet_dts_us = packet_pts_us;
    else if (packet_dts_us >= 0)
        packet_pts_us = packet_dts_us;

    DecodedTaskHolder* holder = nullptr;
    while (!holder)
    {
        holder = AcquireDecodedTaskHolder();
        if (holder)
            break;
        std::this_thread::sleep_for(kHolderWaitInterval);
        DrainPendingRecycleQueue();
    }

    MPP_RET     ret                 = MPP_NOK;
    MppPacket   packet_local        = nullptr;
    MppFrame    decoded_frame       = nullptr;
    MppFrame    out_frm_local       = nullptr;
    MppBuffer   out_buf_local       = nullptr;
    MppTask     input_task          = nullptr;
    MppTask     output_task         = nullptr;
    MppBuffer   decoded_buf         = nullptr;
    bool        is_normal_no_frame  = false;
    const char* fail_stage          = "unknown";
    int         outbuf_fd           = -1;
    size_t      mapped_buffer_index = 0;
    IO_FD_t*    output_info         = nullptr;
    RK_S32      frame_width         = 0;
    RK_S32      frame_height        = 0;
    RK_S32      frame_hs            = 0;
    RK_S32      frame_vs            = 0;
    RK_S32      frame_fmt           = 0;

    auto prepare_info_change = [&]() -> bool
    {
        if (!decoded_frame)
        {
            fail_stage = "info_change_frame_null";
            return false;
        }

        if (is_mjpeg)
        {
            if (group_)
            {
                ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_EXT_BUF_GROUP, group_);
                if (ret != MPP_OK)
                {
                    fail_stage = "info_change_set_ext_buf_group";
                    return false;
                }
            }
        }
        else
        {
            const size_t required_buf_size = mpp_frame_get_buf_size(decoded_frame);
            if (!group_)
            {
                ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM);
                if (ret != MPP_OK || !group_)
                {
                    fail_stage = "info_change_get_internal_group";
                    return false;
                }
            }
            else
            {
                (void)mpp_buffer_group_clear(group_);
            }

            ret = mpp_buffer_group_limit_config(group_, required_buf_size,
                                                resource_limits::kMppOutputBufferCount);
            if (ret != MPP_OK)
            {
                fail_stage = "info_change_limit_group";
                return false;
            }

            ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_EXT_BUF_GROUP, group_);
            if (ret != MPP_OK)
            {
                fail_stage = "info_change_set_internal_group";
                return false;
            }
        }

        ret = mpp_api_->control(mpp_ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (ret != MPP_OK)
        {
            fail_stage = "info_change_ready";
            return false;
        }

        mpp_frame_deinit(&decoded_frame);
        decoded_frame = nullptr;
        return true;
    };

    if (is_mjpeg)
    {
        const int  buffer_index = frame_desc->index;
        MppBuffer  input_buffer = nullptr;
        const bool can_use_imported_dmabuf =
            (frame_desc->fd >= 0 && buffer_index >= 0 &&
             static_cast<size_t>(buffer_index) < resource_limits::kMppImportBufferCount &&
             MppBuffers[buffer_index] != nullptr);

        if (can_use_imported_dmabuf)
        {
            input_buffer = MppBuffers[buffer_index];
            if (effective_size > mpp_buffer_get_size(input_buffer))
            {
                fail_stage = "input_packet_exceeds_imported_buffer";
                goto fail;
            }
        }

        ret = mpp_buffer_get(group_, &out_buf_local, OutSize_);
        if (ret != MPP_OK || !out_buf_local)
        {
            fail_stage = "alloc_output_buffer";
            goto fail;
        }

        ret = mpp_frame_init(&out_frm_local);
        if (ret != MPP_OK || !out_frm_local)
        {
            fail_stage = "init_output_frame";
            goto fail;
        }

        mpp_frame_set_width(out_frm_local, ImgWidth_);
        mpp_frame_set_height(out_frm_local, ImgHeight_);
        mpp_frame_set_hor_stride(out_frm_local, H_Stride_);
        mpp_frame_set_ver_stride(out_frm_local, V_Stride_);
        mpp_frame_set_fmt(out_frm_local, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(out_frm_local, out_buf_local);

        if (can_use_imported_dmabuf)
        {
            ret = mpp_packet_init_with_buffer(&packet_local, input_buffer);
            if (ret != MPP_OK || !packet_local)
            {
                fail_stage = "init_input_packet_imported_dmabuf";
                goto fail;
            }
            mpp_packet_set_data(packet_local, const_cast<void*>(frame_desc->base));
            mpp_packet_set_pos(packet_local, const_cast<void*>(frame_desc->base));
            mpp_packet_set_size(packet_local, effective_size);
            mpp_packet_set_length(packet_local, effective_size);
        }
        else
        {
            ret =
                mpp_packet_init(&packet_local, const_cast<void*>(frame_desc->base), effective_size);
            if (ret != MPP_OK || !packet_local)
            {
                fail_stage = "init_input_packet";
                goto fail;
            }
            mpp_packet_set_data(packet_local, const_cast<void*>(frame_desc->base));
            mpp_packet_set_pos(packet_local, const_cast<void*>(frame_desc->base));
            mpp_packet_set_size(packet_local, effective_size);
            mpp_packet_set_length(packet_local, effective_size);
        }

        mpp_packet_set_pts(packet_local, packet_pts_us);
        mpp_packet_set_dts(packet_local, packet_dts_us);

        ret = mpp_api_->poll(mpp_ctx_, MPP_PORT_INPUT, kPollTimeout500Ms);
        if (ret == MPP_ERR_TIMEOUT || ret < 0)
        {
            fail_stage         = "poll_input_before_submit";
            is_normal_no_frame = true;
            goto fail;
        }

        ret = mpp_api_->dequeue(mpp_ctx_, MPP_PORT_INPUT, &input_task);
        if (ret != MPP_OK || !input_task)
        {
            fail_stage         = "dequeue_input_before_submit";
            is_normal_no_frame = true;
            goto fail;
        }

        ReleaseTaskPacket(input_task);
        ret = mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, packet_local);
        if (ret != MPP_OK)
        {
            fail_stage = "set_meta_input_packet";
            goto fail;
        }

        ret = mpp_task_meta_set_frame(input_task, KEY_OUTPUT_FRAME, out_frm_local);
        if (ret != MPP_OK)
        {
            fail_stage = "set_meta_output_frame";
            goto fail;
        }

        ret = mpp_api_->enqueue(mpp_ctx_, MPP_PORT_INPUT, input_task);
        if (ret != MPP_OK)
        {
            fail_stage = "enqueue_input_submit";
            goto fail;
        }
        input_task   = nullptr;
        packet_local = nullptr;

        ret = mpp_api_->poll(mpp_ctx_, MPP_PORT_OUTPUT, kPollTimeout500Ms);
        if (ret == MPP_ERR_TIMEOUT || ret < 0)
        {
            fail_stage         = "poll_output_after_submit";
            is_normal_no_frame = true;
            goto fail;
        }

        ret = mpp_api_->dequeue(mpp_ctx_, MPP_PORT_OUTPUT, &output_task);
        if (ret != MPP_OK || !output_task)
        {
            fail_stage         = "dequeue_output_after_poll";
            is_normal_no_frame = true;
            goto fail;
        }

        ret = mpp_task_meta_get_frame(output_task, KEY_OUTPUT_FRAME, &decoded_frame);
        if (ret != MPP_OK || !decoded_frame)
        {
            fail_stage = "get_meta_output_frame";
            goto fail;
        }
    }
    else
    {
        ret = mpp_packet_init(&packet_local, const_cast<void*>(frame_desc->base), effective_size);
        if (ret != MPP_OK || !packet_local)
        {
            fail_stage = "init_input_packet";
            goto fail;
        }
        mpp_packet_set_data(packet_local, const_cast<void*>(frame_desc->base));
        mpp_packet_set_pos(packet_local, const_cast<void*>(frame_desc->base));
        mpp_packet_set_size(packet_local, effective_size);
        mpp_packet_set_length(packet_local, effective_size);
        mpp_packet_set_pts(packet_local, packet_pts_us);
        mpp_packet_set_dts(packet_local, packet_dts_us);

        for (int retry = 0; retry < kDecodePutRetryCnt; ++retry)
        {
            ret = mpp_api_->decode_put_packet(mpp_ctx_, packet_local);
            if (ret == MPP_OK)
                break;
            std::this_thread::sleep_for(kDecodeRetrySleep);
        }
        if (ret != MPP_OK)
        {
            fail_stage = "decode_put_packet";
            goto fail;
        }

        if (packet_local)
        {
            mpp_packet_deinit(&packet_local);
            packet_local = nullptr;
        }

        for (int retry = 0; retry < kDecodeGetRetryCnt; ++retry)
        {
            ret = mpp_api_->decode_get_frame(mpp_ctx_, &decoded_frame);
            if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT)
            {
                fail_stage = "decode_get_frame";
                goto fail;
            }
            if (!decoded_frame)
            {
                std::this_thread::sleep_for(kDecodeRetrySleep);
                continue;
            }
            if (mpp_frame_get_info_change(decoded_frame))
            {
                if (!prepare_info_change())
                    goto fail;
                std::this_thread::sleep_for(kDecodeRetrySleep);
                continue;
            }
            if (mpp_frame_get_errinfo(decoded_frame) || mpp_frame_get_discard(decoded_frame))
            {
                mpp_frame_deinit(&decoded_frame);
                decoded_frame = nullptr;
                std::this_thread::sleep_for(kDecodeRetrySleep);
                continue;
            }
            break;
        }

        if (!decoded_frame)
        {
            is_normal_no_frame = true;
            goto fail;
        }
    }

    if (mpp_frame_get_info_change(decoded_frame))
    {
        if (!prepare_info_change())
            goto fail;
        is_normal_no_frame = true;
        goto fail;
    }

    if (mpp_frame_get_errinfo(decoded_frame) || mpp_frame_get_discard(decoded_frame))
    {
        is_normal_no_frame = true;
        goto fail;
    }

    decoded_buf = mpp_frame_get_buffer(decoded_frame);
    if (!decoded_buf)
    {
        fail_stage = "get_decoded_buffer";
        goto fail;
    }

    outbuf_fd = mpp_buffer_get_fd(decoded_buf);
    if (outbuf_fd < 0)
    {
        fail_stage = "get_decoded_fd";
        goto fail;
    }

    if (is_mjpeg)
    {
        const auto out_it = OutBufFD2Index_Map_.find(outbuf_fd);
        if (out_it == OutBufFD2Index_Map_.end())
        {
            fail_stage = "map_output_fd_to_index";
            goto fail;
        }
        mapped_buffer_index = out_it->second;
    }
    else
    {
        mapped_buffer_index = static_cast<size_t>(holder - &HolderPool_[0]);
    }

    if (mapped_buffer_index >= resource_limits::kMppOutputBufferCount)
    {
        fail_stage = "mapped_output_index_out_of_range";
        goto fail;
    }

    output_info = &MppOutputFDList_[mapped_buffer_index];
    if (!is_mjpeg)
    {
        output_info->fd   = outbuf_fd;
        output_info->base = nullptr;
        output_info->size = mpp_buffer_get_size(decoded_buf);
    }

    frame_width  = mpp_frame_get_width(decoded_frame);
    frame_height = mpp_frame_get_height(decoded_frame);
    frame_hs     = mpp_frame_get_hor_stride(decoded_frame);
    frame_vs     = mpp_frame_get_ver_stride(decoded_frame);
    frame_fmt    = static_cast<RK_S32>(mpp_frame_get_fmt(decoded_frame) & MPP_FRAME_FMT_MASK);
    if (frame_width <= 0 || frame_height <= 0 || frame_hs <= 0 || frame_vs <= 0)
    {
        fail_stage = "invalid_frame_geometry";
        goto fail;
    }

    output_info->format     = static_cast<uint32_t>(frame_fmt);
    output_info->width      = static_cast<uint32_t>(frame_width);
    output_info->height     = static_cast<uint32_t>(frame_height);
    output_info->hor_stride = static_cast<uint32_t>(frame_hs);
    output_info->ver_stride = static_cast<uint32_t>(frame_vs);
    output_info->pts_us     = mpp_frame_get_pts(decoded_frame);
    output_info->dts_us     = mpp_frame_get_dts(decoded_frame);
    if (output_info->pts_us >= 0)
        output_info->dts_us = output_info->pts_us;
    else if (output_info->dts_us >= 0)
        output_info->pts_us = output_info->dts_us;

    if (output_info->size == 0)
    {
        output_info->size = static_cast<size_t>(output_info->hor_stride) *
                            static_cast<size_t>(output_info->ver_stride) * 3u / 2u;
    }

    if (is_mjpeg)
    {
        ret = mpp_api_->enqueue(mpp_ctx_, MPP_PORT_OUTPUT, output_task);
        if (ret != MPP_OK)
        {
            fail_stage = "enqueue_output_return";
            goto fail;
        }
        output_task = nullptr;

        ret = mpp_api_->poll(mpp_ctx_, MPP_PORT_INPUT, kPollTimeout500Ms);
        if (ret >= 0)
        {
            ret = mpp_api_->dequeue(mpp_ctx_, MPP_PORT_INPUT, &input_task);
            if (ret == MPP_OK && input_task)
            {
                ReleaseTaskPacket(input_task);
                if (mpp_api_->enqueue(mpp_ctx_, MPP_PORT_INPUT, input_task) != MPP_OK)
                {
                    fail_stage = "enqueue_input_reclaimed_task";
                    goto fail;
                }
                input_task = nullptr;
            }
        }

        holder->output_desc  = output_info;
        holder->output_frame = out_frm_local;
        holder->output_buf   = out_buf_local;
    }
    else
    {
        holder->output_desc  = output_info;
        holder->output_frame = decoded_frame;
        holder->output_buf   = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(HolderMutex_);
        if (OutDesc2HolderMap_.find(holder->output_desc) != OutDesc2HolderMap_.end())
        {
            fail_stage = "output_desc_inflight";
            goto fail;
        }
        OutDesc2HolderMap_[holder->output_desc] = holder;
    }

    CurrentOutputDesc = holder->output_desc;
    if (is_mjpeg)
    {
        decoded_frame = nullptr;
        out_frm_local = nullptr;
        out_buf_local = nullptr;
    }
    else
    {
        decoded_frame = nullptr;
    }
    holder = nullptr;
    return 0;

fail:
    if (!is_normal_no_frame)
    {
        std::cerr << "[MPPDEC][FAIL] stage=" << fail_stage << ", ret=" << static_cast<int>(ret)
                  << ", payload=" << effective_size << ", index=" << frame_desc->index
                  << ", fd=" << frame_desc->fd << ", errno=" << errno << " ("
                  << std::strerror(errno) << ")" << std::endl;
    }

    CurrentOutputDesc = nullptr;
    if (output_task)
    {
        (void)mpp_api_->enqueue(mpp_ctx_, MPP_PORT_OUTPUT, output_task);
        output_task = nullptr;
    }
    if (input_task)
    {
        ReleaseTaskPacket(input_task);
        (void)mpp_api_->enqueue(mpp_ctx_, MPP_PORT_INPUT, input_task);
        input_task = nullptr;
    }
    if (packet_local)
        mpp_packet_deinit(&packet_local);
    if (decoded_frame)
        mpp_frame_deinit(&decoded_frame);
    if (out_frm_local)
        mpp_frame_deinit(&out_frm_local);
    if (out_buf_local)
        mpp_buffer_put(out_buf_local);
    ReturnDecodedTaskHolder(holder);
    return is_normal_no_frame ? 0 : -1;
}
int MppDecInstance::AllocDmaBufFD(IO_FD_t& output, size_t size)
{
    if (size == 0)
        return -1;
    int heap = open(mpp_common::kSystemDmaHeapPath, O_RDWR | O_CLOEXEC);
    if (heap < 0)
        return -1;

    dma_heap_allocation_data req{};
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
    {
        close(heap);
        return -1;
    }
    close(heap);

    void* mapped_base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped_base == MAP_FAILED)
    {
        close(req.fd);
        return -1;
    }

    output.fd         = req.fd;
    output.base       = mapped_base;
    output.size       = size;
    output.format     = 0;
    output.width      = 0;
    output.height     = 0;
    output.hor_stride = 0;
    output.ver_stride = 0;
    output.pts_us     = -1;
    output.dts_us     = -1;
    return 0;
}

void MppDecInstance::ReleaseExternalOutputBuffers()
{
    ForceRecycleAllHolders();
    DrainPendingRecycleQueue();

    for (auto& output : MppOutputFDList_)
    {
        if (output.base && output.size > 0)
        {
            munmap(output.base, output.size);
            output.base = nullptr;
            output.size = 0;
        }
        if (output.fd >= 0)
        {
            close(output.fd);
            output.fd = -1;
        }

        output.format     = 0;
        output.width      = 0;
        output.height     = 0;
        output.hor_stride = 0;
        output.ver_stride = 0;
        output.pts_us     = -1;
        output.dts_us     = -1;
    }

    OutBufFD2Index_Map_.clear();
    CurrentOutputDesc = nullptr;
}

int MppDecInstance::CommitExternalOutputBuffers(size_t size)
{
    if (!group_ || size == 0)
        return -1;
    {
        std::lock_guard<std::mutex> lock(HolderMutex_);
        if (!OutDesc2HolderMap_.empty())
            return -1;
    }

    DrainPendingRecycleQueue();
    if (mpp_buffer_group_clear(group_) != MPP_OK)
        return -1;
    ReleaseExternalOutputBuffers();

    MppBufferInfo commit{};
    commit.type = MPP_BUFFER_TYPE_EXT_DMA;
    commit.size = size;

    for (size_t i = 0; i < resource_limits::kMppOutputBufferCount; ++i)
    {
        IO_FD_t& output = MppOutputFDList_[i];
        if (AllocDmaBufFD(output, size) != 0)
        {
            (void)mpp_buffer_group_clear(group_);
            ReleaseExternalOutputBuffers();
            return -1;
        }
        OutBufFD2Index_Map_[output.fd] = i;
        commit.fd                      = output.fd;
        commit.ptr                     = nullptr;
        commit.index                   = static_cast<int>(i);
        if (mpp_buffer_commit(group_, &commit) != MPP_OK)
        {
            (void)mpp_buffer_group_clear(group_);
            ReleaseExternalOutputBuffers();
            return -1;
        }
    }
    return 0;
}
