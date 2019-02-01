/*
 * Copyright (C) 2017-2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/built_ins/built_ins.h"
#include "runtime/built_ins/builtins_dispatch_builder.h"
#include "reg_configs_common.h"
#include "runtime/helpers/cache_policy.h"
#include "runtime/helpers/dispatch_info.h"
#include "runtime/memory_manager/allocations_list.h"
#include "unit_tests/command_queue/enqueue_fixture.h"
#include "unit_tests/gen_common/gen_commands_common_validation.h"
#include "unit_tests/command_queue/enqueue_read_buffer_fixture.h"
#include "unit_tests/helpers/debug_manager_state_restore.h"
#include "unit_tests/helpers/unit_test_helper.h"
#include "unit_tests/mocks/mock_command_queue.h"
#include "test.h"

using namespace OCLRT;

HWTEST_F(EnqueueReadBufferTypeTest, null_mem_object) {
    auto data = 1;
    auto retVal = clEnqueueReadBuffer(
        pCmdQ,
        nullptr,
        false,
        0,
        sizeof(data),
        &data,
        0,
        nullptr,
        nullptr);

    EXPECT_EQ(CL_INVALID_MEM_OBJECT, retVal);
}

HWTEST_F(EnqueueReadBufferTypeTest, null_user_pointer) {
    auto data = 1;

    auto retVal = clEnqueueReadBuffer(
        pCmdQ,
        srcBuffer.get(),
        false,
        0,
        sizeof(data),
        nullptr,
        0,
        nullptr,
        nullptr);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, GPGPUWalker) {
    typedef typename FamilyType::GPGPU_WALKER GPGPU_WALKER;

    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();

    ASSERT_NE(cmdList.end(), itorWalker);
    auto *cmd = (GPGPU_WALKER *)*itorWalker;

    // Verify GPGPU_WALKER parameters
    EXPECT_NE(0u, cmd->getThreadGroupIdXDimension());
    EXPECT_NE(0u, cmd->getThreadGroupIdYDimension());
    EXPECT_NE(0u, cmd->getThreadGroupIdZDimension());
    EXPECT_NE(0u, cmd->getRightExecutionMask());
    EXPECT_NE(0u, cmd->getBottomExecutionMask());
    EXPECT_EQ(GPGPU_WALKER::SIMD_SIZE_SIMD32, cmd->getSimdSize());
    EXPECT_NE(0u, cmd->getIndirectDataLength());
    EXPECT_FALSE(cmd->getIndirectParameterEnable());

    // Compute the SIMD lane mask
    size_t simd =
        cmd->getSimdSize() == GPGPU_WALKER::SIMD_SIZE_SIMD32 ? 32 : cmd->getSimdSize() == GPGPU_WALKER::SIMD_SIZE_SIMD16 ? 16 : 8;
    uint64_t simdMask = (1ull << simd) - 1;

    // Mask off lanes based on the execution masks
    auto laneMaskRight = cmd->getRightExecutionMask() & simdMask;
    auto lanesPerThreadX = 0;
    while (laneMaskRight) {
        lanesPerThreadX += laneMaskRight & 1;
        laneMaskRight >>= 1;
    }
}

HWTEST_F(EnqueueReadBufferTypeTest, bumpsTaskLevel) {
    auto taskLevelBefore = pCmdQ->taskLevel;

    srcBuffer->forceDisallowCPUCopy = true;
    EnqueueReadBufferHelper<>::enqueueReadBuffer(pCmdQ, srcBuffer.get(), CL_TRUE);
    EXPECT_GT(pCmdQ->taskLevel, taskLevelBefore);
}

HWTEST_F(EnqueueReadBufferTypeTest, alignsToCSR_Blocking) {
    //this test case assumes IOQ
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    csr.taskCount = pCmdQ->taskCount + 100;
    csr.taskLevel = pCmdQ->taskLevel + 50;
    auto oldCsrTaskLevel = csr.peekTaskLevel();

    srcBuffer->forceDisallowCPUCopy = true;
    EnqueueReadBufferHelper<>::enqueueReadBuffer(pCmdQ, srcBuffer.get(), CL_TRUE);
    EXPECT_EQ(csr.peekTaskCount(), pCmdQ->taskCount);
    EXPECT_EQ(oldCsrTaskLevel, pCmdQ->taskLevel);
}

HWTEST_F(EnqueueReadBufferTypeTest, alignsToCSR_NonBlocking) {
    //this test case assumes IOQ
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    csr.taskCount = pCmdQ->taskCount + 100;
    csr.taskLevel = pCmdQ->taskLevel + 50;

    EnqueueReadBufferHelper<>::enqueueReadBuffer(pCmdQ, srcBuffer.get(), CL_FALSE);
    EXPECT_EQ(csr.peekTaskCount(), pCmdQ->taskCount);
    EXPECT_EQ(csr.peekTaskLevel(), pCmdQ->taskLevel + 1);
}

HWTEST_F(EnqueueReadBufferTypeTest, addsCommands) {
    auto usedCmdBufferBefore = pCS->getUsed();

    srcBuffer->forceDisallowCPUCopy = true;
    EnqueueReadBufferHelper<>::enqueueReadBuffer(pCmdQ, srcBuffer.get(), CL_TRUE);
    EXPECT_NE(usedCmdBufferBefore, pCS->getUsed());
}

HWTEST_F(EnqueueReadBufferTypeTest, addsIndirectData) {
    auto dshBefore = pDSH->getUsed();
    auto iohBefore = pIOH->getUsed();
    auto sshBefore = pSSH->getUsed();

    srcBuffer->forceDisallowCPUCopy = true;
    EnqueueReadBufferHelper<>::enqueueReadBuffer(pCmdQ, srcBuffer.get(), CL_TRUE);

    MultiDispatchInfo multiDispatchInfo;
    auto &builder = pCmdQ->getDevice().getExecutionEnvironment()->getBuiltIns()->getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer,
                                                                                                               pCmdQ->getContext(), pCmdQ->getDevice());
    ASSERT_NE(nullptr, &builder);

    BuiltinDispatchInfoBuilder::BuiltinOpParams dc;
    dc.dstPtr = EnqueueReadBufferTraits::hostPtr;
    dc.srcMemObj = srcBuffer.get();
    dc.srcOffset = {EnqueueReadBufferTraits::offset, 0, 0};
    dc.size = {srcBuffer->getSize(), 0, 0};
    builder.buildDispatchInfos(multiDispatchInfo, dc);
    EXPECT_NE(0u, multiDispatchInfo.size());

    auto kernel = multiDispatchInfo.begin()->getKernel();

    EXPECT_TRUE(UnitTestHelper<FamilyType>::evaluateDshUsage(dshBefore, pDSH->getUsed(), kernel));
    EXPECT_NE(iohBefore, pIOH->getUsed());
    if (kernel->requiresSshForBuffers()) {
        EXPECT_NE(sshBefore, pSSH->getUsed());
    }
}

HWTEST_F(EnqueueReadBufferTypeTest, LoadRegisterImmediateL3CNTLREG) {
    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();
    validateL3Programming<FamilyType>(cmdList, itorWalker);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, WhenEnqueueIsDoneThenStateBaseAddressIsProperlyProgrammed) {
    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();
    validateStateBaseAddress<FamilyType>(this->pCmdQ->getCommandStreamReceiver().getMemoryManager()->getInternalHeapBaseAddress(),
                                         pDSH, pIOH, pSSH, itorPipelineSelect, itorWalker, cmdList, 0llu);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, MediaInterfaceDescriptorLoad) {
    typedef typename FamilyType::MEDIA_INTERFACE_DESCRIPTOR_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD;
    typedef typename FamilyType::INTERFACE_DESCRIPTOR_DATA INTERFACE_DESCRIPTOR_DATA;

    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();

    // All state should be programmed before walker
    auto itorCmd = find<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(itorPipelineSelect, itorWalker);
    ASSERT_NE(itorWalker, itorCmd);

    auto *cmd = (MEDIA_INTERFACE_DESCRIPTOR_LOAD *)*itorCmd;

    // Verify we have a valid length -- multiple of INTERFACE_DESCRIPTOR_DATAs
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorTotalLength() % sizeof(INTERFACE_DESCRIPTOR_DATA));

    // Validate the start address
    size_t alignmentStartAddress = 64 * sizeof(uint8_t);
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorDataStartAddress() % alignmentStartAddress);

    // Validate the length
    EXPECT_NE(0u, cmd->getInterfaceDescriptorTotalLength());
    size_t alignmentTotalLength = 32 * sizeof(uint8_t);
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorTotalLength() % alignmentTotalLength);

    // Generically validate this command
    FamilyType::PARSE::template validateCommand<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(cmdList.begin(), itorCmd);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, InterfaceDescriptorData) {
    typedef typename FamilyType::MEDIA_INTERFACE_DESCRIPTOR_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD;
    typedef typename FamilyType::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;
    typedef typename FamilyType::INTERFACE_DESCRIPTOR_DATA INTERFACE_DESCRIPTOR_DATA;

    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();

    // Extract the MIDL command
    auto itorCmd = find<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(itorPipelineSelect, itorWalker);
    ASSERT_NE(itorWalker, itorCmd);
    auto *cmdMIDL = (MEDIA_INTERFACE_DESCRIPTOR_LOAD *)*itorCmd;

    // Extract the SBA command
    itorCmd = find<STATE_BASE_ADDRESS *>(cmdList.begin(), itorWalker);
    ASSERT_NE(itorWalker, itorCmd);
    auto *cmdSBA = (STATE_BASE_ADDRESS *)*itorCmd;

    // Extrach the DSH
    auto DSH = cmdSBA->getDynamicStateBaseAddress();
    ASSERT_NE(0u, DSH);

    // IDD should be located within DSH
    auto iddStart = cmdMIDL->getInterfaceDescriptorDataStartAddress();
    auto IDDEnd = iddStart + cmdMIDL->getInterfaceDescriptorTotalLength();
    ASSERT_LE(IDDEnd, cmdSBA->getDynamicStateBufferSize() * MemoryConstants::pageSize);

    auto &IDD = *(INTERFACE_DESCRIPTOR_DATA *)cmdInterfaceDescriptorData;

    // Validate the kernel start pointer.  Technically, a kernel can start at address 0 but let's force a value.
    auto kernelStartPointer = ((uint64_t)IDD.getKernelStartPointerHigh() << 32) + IDD.getKernelStartPointer();
    EXPECT_LE(kernelStartPointer, cmdSBA->getInstructionBufferSize() * MemoryConstants::pageSize);

    EXPECT_NE(0u, IDD.getNumberOfThreadsInGpgpuThreadGroup());
    EXPECT_NE(0u, IDD.getCrossThreadConstantDataReadLength());
    EXPECT_NE(0u, IDD.getConstantIndirectUrbEntryReadLength());
}

HWTEST_F(EnqueueReadBufferTypeTest, PipelineSelect) {
    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();
    int numCommands = getNumberOfPipelineSelectsThatEnablePipelineSelect<FamilyType>();
    EXPECT_EQ(1, numCommands);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, MediaVFEState) {
    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>();
    validateMediaVFEState<FamilyType>(&pDevice->getHardwareInfo(), cmdMediaVfeState, cmdList, itorMediaVfeState);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueReadBufferTypeTest, blockingRequiresPipeControlAfterWalkerWithDCFlushSet) {
    typedef typename FamilyType::PIPE_CONTROL PIPE_CONTROL;

    srcBuffer->forceDisallowCPUCopy = true;
    enqueueReadBuffer<FamilyType>(CL_TRUE);

    // All state should be programmed after walker
    auto itorWalker = find<typename FamilyType::GPGPU_WALKER *>(cmdList.begin(), cmdList.end());
    auto itorCmd = find<PIPE_CONTROL *>(itorWalker, cmdList.end());
    auto *cmd = (PIPE_CONTROL *)*itorCmd;
    EXPECT_NE(cmdList.end(), itorCmd);

    if (::renderCoreFamily != IGFX_GEN8_CORE) {
        // SKL+: two PIPE_CONTROLs following GPGPU_WALKER: first has DcFlush and second has Write HwTag
        EXPECT_TRUE(cmd->getDcFlushEnable());
        // Move to next PPC
        auto itorCmdP = ++((GenCmdList::iterator)itorCmd);
        EXPECT_NE(cmdList.end(), itorCmdP);
        auto itorCmd2 = find<PIPE_CONTROL *>(itorCmdP, cmdList.end());
        cmd = (PIPE_CONTROL *)*itorCmd2;
        EXPECT_FALSE(cmd->getDcFlushEnable());
    } else {
        // BDW: single PIPE_CONTROL following GPGPU_WALKER has DcFlush and Write HwTag
        EXPECT_TRUE(cmd->getDcFlushEnable());
    }
}

HWTEST_F(EnqueueReadBufferTypeTest, givenAlignedPointerAndAlignedSizeWhenReadBufferIsCalledThenRecordedL3IndexIsL3ON) {
    void *ptr = (void *)0x1040;

    cl_int retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                             CL_FALSE,
                                             0,
                                             MemoryConstants::cacheLineSize,
                                             ptr,
                                             0,
                                             nullptr,
                                             nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    EXPECT_EQ(CacheSettings::l3CacheOn, csr.latestSentStatelessMocsConfig);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenNotAlignedPointerAndAlignedSizeWhenReadBufferIsCalledThenRecordedL3IndexIsL3Off) {
    void *ptr = (void *)0x1039;

    cl_int retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                             CL_FALSE,
                                             0,
                                             MemoryConstants::cacheLineSize,
                                             ptr,
                                             0,
                                             nullptr,
                                             nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    EXPECT_EQ(CacheSettings::l3CacheOff, csr.latestSentStatelessMocsConfig);
    EXPECT_FALSE(csr.disableL3Cache);

    void *ptr2 = (void *)0x1040;

    retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr2,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CacheSettings::l3CacheOn, csr.latestSentStatelessMocsConfig);
    EXPECT_FALSE(csr.disableL3Cache);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenOOQWithEnabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndZeroCopyBufferWhenReadBufferIsExecutedThenTaskLevelNotIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(true);
    cl_int retVal = CL_SUCCESS;
    std::unique_ptr<CommandQueue> pCmdOOQ(createCommandQueue(pDevice, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE));
    void *ptr = srcBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdOOQ->enqueueReadBuffer(srcBuffer.get(),
                                        CL_FALSE,
                                        0,
                                        MemoryConstants::cacheLineSize,
                                        ptr,
                                        0,
                                        nullptr,
                                        nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdOOQ->taskLevel, 0u);
}
HWTEST_F(EnqueueReadBufferTypeTest, givenOOQWithDisabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndZeroCopyBufferWhenReadBufferIsExecutedThenTaskLevelNotIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(false);
    cl_int retVal = CL_SUCCESS;
    std::unique_ptr<CommandQueue> pCmdOOQ(createCommandQueue(pDevice, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE));
    void *ptr = srcBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdOOQ->enqueueReadBuffer(srcBuffer.get(),
                                        CL_FALSE,
                                        0,
                                        MemoryConstants::cacheLineSize,
                                        ptr,
                                        0,
                                        nullptr,
                                        nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdOOQ->taskLevel, 0u);
}
HWTEST_F(EnqueueReadBufferTypeTest, givenInOrderQueueAndEnabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndZeroCopyBufferWhenReadBufferIsExecutedThenTaskLevelShouldNotBeIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(true);
    cl_int retVal = CL_SUCCESS;
    void *ptr = srcBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdQ->taskLevel, 0u);
}
HWTEST_F(EnqueueReadBufferTypeTest, givenInOrderQueueAndDisabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndZeroCopyBufferWhenReadBufferIsExecutedThenTaskLevelShouldNotBeIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(false);
    cl_int retVal = CL_SUCCESS;
    void *ptr = srcBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdQ->taskLevel, 0u);
}
HWTEST_F(EnqueueReadBufferTypeTest, givenInOrderQueueAndDisabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndNonZeroCopyBufferWhenReadBufferIsExecutedThenTaskLevelShouldBeIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(false);
    cl_int retVal = CL_SUCCESS;
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdQ->enqueueReadBuffer(nonZeroCopyBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdQ->taskLevel, 1u);
}
HWTEST_F(EnqueueReadBufferTypeTest, givenInOrderQueueAndEnabledSupportCpuCopiesAndDstPtrEqualSrcPtrAndNonZeroCopyWhenReadBufferIsExecutedThenTaskLevelShouldBeIncreased) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(true);
    cl_int retVal = CL_SUCCESS;
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();
    EXPECT_EQ(retVal, CL_SUCCESS);
    retVal = pCmdQ->enqueueReadBuffer(nonZeroCopyBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pCmdQ->taskLevel, 1u);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenCommandQueueWhenEnqueueReadBufferIsCalledThenItCallsNotifyFunction) {
    auto mockCmdQ = std::make_unique<MockCommandQueueHw<FamilyType>>(context, pDevice, nullptr);
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();
    auto retVal = mockCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                              CL_TRUE,
                                              0,
                                              MemoryConstants::cacheLineSize,
                                              ptr,
                                              0,
                                              nullptr,
                                              nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(mockCmdQ->notifyEnqueueReadBufferCalled);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenEnqueueReadBufferCalledWhenLockedPtrInTransferPropertisIsAvailableThenItIsNotUnlocked) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(true);

    ExecutionEnvironment executionEnvironment;
    MockMemoryManager memoryManager(false, true, executionEnvironment);
    MockContext ctx;
    cl_int retVal;
    ctx.setMemoryManager(&memoryManager);
    auto mockCmdQ = std::make_unique<MockCommandQueueHw<FamilyType>>(context, pDevice, nullptr);
    std::unique_ptr<Buffer> buffer(Buffer::create(&ctx, 0, 1, nullptr, retVal));
    static_cast<MemoryAllocation *>(buffer->getGraphicsAllocation())->overrideMemoryPool(MemoryPool::SystemCpuInaccessible);
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();

    retVal = mockCmdQ->enqueueReadBuffer(buffer.get(),
                                         CL_TRUE,
                                         0,
                                         MemoryConstants::cacheLineSize,
                                         ptr,
                                         0,
                                         nullptr,
                                         nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(0u, memoryManager.unlockResourceCalled);
}

HWTEST_F(EnqueueReadBufferTypeTest, gicenEnqueueReadBufferCalledWhenLockedPtrInTransferPropertisIsNotAvailableThenItIsNotUnlocked) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.DoCpuCopyOnReadBuffer.set(true);

    ExecutionEnvironment executionEnvironment;
    MockMemoryManager memoryManager(false, true, executionEnvironment);
    MockContext ctx;
    cl_int retVal;
    ctx.setMemoryManager(&memoryManager);
    auto mockCmdQ = std::make_unique<MockCommandQueueHw<FamilyType>>(context, pDevice, nullptr);
    std::unique_ptr<Buffer> buffer(Buffer::create(&ctx, 0, 1, nullptr, retVal));
    static_cast<MemoryAllocation *>(buffer->getGraphicsAllocation())->overrideMemoryPool(MemoryPool::System4KBPages);
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();

    retVal = mockCmdQ->enqueueReadBuffer(buffer.get(),
                                         CL_TRUE,
                                         0,
                                         MemoryConstants::cacheLineSize,
                                         ptr,
                                         0,
                                         nullptr,
                                         nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(0u, memoryManager.unlockResourceCalled);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenEnqueueReadBufferBlockingWhenAUBDumpAllocsOnEnqueueReadOnlyIsOnThenBufferShouldBeSetDumpable) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.AUBDumpAllocsOnEnqueueReadOnly.set(true);

    ASSERT_FALSE(srcBuffer->getGraphicsAllocation()->isAllocDumpable());
    cl_int retVal = CL_SUCCESS;
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();
    retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                      CL_TRUE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(srcBuffer->getGraphicsAllocation()->isAllocDumpable());
    EXPECT_TRUE(srcBuffer->forceDisallowCPUCopy);
}

HWTEST_F(EnqueueReadBufferTypeTest, givenEnqueueReadBufferNonBlockingWhenAUBDumpAllocsOnEnqueueReadOnlyIsOnThenBufferShouldntBeSetDumpable) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.AUBDumpAllocsOnEnqueueReadOnly.set(true);

    ASSERT_FALSE(srcBuffer->getGraphicsAllocation()->isAllocDumpable());
    cl_int retVal = CL_SUCCESS;
    void *ptr = nonZeroCopyBuffer->getCpuAddressForMemoryTransfer();
    retVal = pCmdQ->enqueueReadBuffer(srcBuffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_FALSE(srcBuffer->getGraphicsAllocation()->isAllocDumpable());
    EXPECT_FALSE(srcBuffer->forceDisallowCPUCopy);
}

using NegativeFailAllocationTest = Test<NegativeFailAllocationCommandEnqueueBaseFixture>;

HWTEST_F(NegativeFailAllocationTest, givenEnqueueReadBufferWhenHostPtrAllocationCreationFailsThenReturnOutOfResource) {
    cl_int retVal = CL_SUCCESS;
    retVal = pCmdQ->enqueueReadBuffer(buffer.get(),
                                      CL_FALSE,
                                      0,
                                      MemoryConstants::cacheLineSize,
                                      ptr,
                                      0,
                                      nullptr,
                                      nullptr);

    EXPECT_EQ(CL_OUT_OF_RESOURCES, retVal);
}
