// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2022.2 (64-bit)
// Tool Version Limit: 2019.12
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// ==============================================================
#ifndef XDEHAZE_H
#define XDEHAZE_H

#ifdef __cplusplus
extern "C" {
#endif

/***************************** Include Files *********************************/
#ifndef __linux__
#include "xil_types.h"
#include "xil_assert.h"
#include "xstatus.h"
#include "xil_io.h"
#else
#include <stdint.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#endif
#include "xdehaze_hw.h"

/**************************** Type Definitions ******************************/
#ifdef __linux__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
typedef struct {
    u16 DeviceId;
    u64 Ctrl_BaseAddress;
} XDehaze_Config;
#endif

typedef struct {
    u64 Ctrl_BaseAddress;
    u32 IsReady;
} XDehaze;

typedef u32 word_type;

/***************** Macros (Inline Functions) Definitions *********************/
#ifndef __linux__
#define XDehaze_WriteReg(BaseAddress, RegOffset, Data) \
    Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))
#define XDehaze_ReadReg(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))
#else
#define XDehaze_WriteReg(BaseAddress, RegOffset, Data) \
    *(volatile u32*)((BaseAddress) + (RegOffset)) = (u32)(Data)
#define XDehaze_ReadReg(BaseAddress, RegOffset) \
    *(volatile u32*)((BaseAddress) + (RegOffset))

#define Xil_AssertVoid(expr)    assert(expr)
#define Xil_AssertNonvoid(expr) assert(expr)

#define XST_SUCCESS             0
#define XST_DEVICE_NOT_FOUND    2
#define XST_OPEN_DEVICE_FAILED  3
#define XIL_COMPONENT_IS_READY  1
#endif

/************************** Function Prototypes *****************************/
#ifndef __linux__
int XDehaze_Initialize(XDehaze *InstancePtr, u16 DeviceId);
XDehaze_Config* XDehaze_LookupConfig(u16 DeviceId);
int XDehaze_CfgInitialize(XDehaze *InstancePtr, XDehaze_Config *ConfigPtr);
#else
int XDehaze_Initialize(XDehaze *InstancePtr, const char* InstanceName);
int XDehaze_Release(XDehaze *InstancePtr);
#endif

void XDehaze_Start(XDehaze *InstancePtr);
u32 XDehaze_IsDone(XDehaze *InstancePtr);
u32 XDehaze_IsIdle(XDehaze *InstancePtr);
u32 XDehaze_IsReady(XDehaze *InstancePtr);
void XDehaze_EnableAutoRestart(XDehaze *InstancePtr);
void XDehaze_DisableAutoRestart(XDehaze *InstancePtr);

void XDehaze_Set_ctrl_reg(XDehaze *InstancePtr, u32 Data);
u32 XDehaze_Get_ctrl_reg(XDehaze *InstancePtr);
void XDehaze_Set_threshold_reg(XDehaze *InstancePtr, u32 Data);
u32 XDehaze_Get_threshold_reg(XDehaze *InstancePtr);

void XDehaze_InterruptGlobalEnable(XDehaze *InstancePtr);
void XDehaze_InterruptGlobalDisable(XDehaze *InstancePtr);
void XDehaze_InterruptEnable(XDehaze *InstancePtr, u32 Mask);
void XDehaze_InterruptDisable(XDehaze *InstancePtr, u32 Mask);
void XDehaze_InterruptClear(XDehaze *InstancePtr, u32 Mask);
u32 XDehaze_InterruptGetEnabled(XDehaze *InstancePtr);
u32 XDehaze_InterruptGetStatus(XDehaze *InstancePtr);

#ifdef __cplusplus
}
#endif

#endif
