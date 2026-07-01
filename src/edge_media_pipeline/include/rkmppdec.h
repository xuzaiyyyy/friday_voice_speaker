#ifndef RKMPPDEC_H
#define RKMPPDEC_H

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>

#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>

#include "pubDataType.h"

class MppDecInstance
{
   public:
    MppDecInstance();
    ~MppDecInstance();

    int DecInit(MppCodingType coding_type = MPP_VIDEO_CodingMJPEG);
    int DecAllocBuffer(const FrameDesc* frame_desc_array, size_t frame_desc_count);
    int DecConfigWidthHeight(uint32_t width, uint32_t height);
    int ResetDecoder();
    int MppDecode(const FrameDesc* frame_desc);
    int DecQueueOutputForRecycle(const IO_FD_t* output_desc);

    MppBuffer      MppBuffers[resource_limits::kMppImportBufferCount] = {nullptr};
    const IO_FD_t* CurrentOutputDesc                                  = nullptr;

   private:
    struct DecodedTaskHolder
    {
        IO_FD_t*  output_desc  = nullptr;
        MppFrame  output_frame = nullptr;
        MppBuffer output_buf   = nullptr;
    };

    int                AllocDmaBufFD(IO_FD_t& output, size_t size);
    int                CommitExternalOutputBuffers(size_t size);
    void               ReleaseExternalOutputBuffers();
    DecodedTaskHolder* AcquireDecodedTaskHolder();
    void               ReturnDecodedTaskHolder(DecodedTaskHolder* holder);
    void               ReleaseTaskPacket(MppTask task);
    void               RecycleDecodedTaskHolder(DecodedTaskHolder* holder);
    void               DrainPendingRecycleQueue();
    void               ForceRecycleAllHolders();

    MppCtx                mpp_ctx_      = nullptr;
    MppApi*               mpp_api_      = nullptr;
    MppBufferGroup        group_        = nullptr;
    MppBufferGroup        packet_group_ = nullptr;
    MppCodingType         coding_type_  = MPP_VIDEO_CodingMJPEG;
    uint32_t              ImgWidth_     = 0;
    uint32_t              ImgHeight_    = 0;
    uint32_t              H_Stride_     = 0;
    uint32_t              V_Stride_     = 0;
    size_t                OutSize_      = 0;
    IO_FD_t               MppOutputFDList_[resource_limits::kMppOutputBufferCount] = {};
    std::map<int, size_t> OutBufFD2Index_Map_;
    std::array<DecodedTaskHolder, resource_limits::kMppOutputBufferCount> HolderPool_ = {};
    std::deque<DecodedTaskHolder*>                                        FreeHolderQueue_;
    std::deque<DecodedTaskHolder*>                                        PendingRecycleQueue_;
    std::map<const IO_FD_t*, DecodedTaskHolder*>                          OutDesc2HolderMap_;
    std::mutex                                                            HolderMutex_;
};

#endif  // RKMPPDEC_H

