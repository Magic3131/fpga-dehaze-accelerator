// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2022.2 (64-bit)
// Tool Version Limit: 2019.12
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// ==============================================================
/***************************** Include Files *********************************/
#include "xdehaze.h"

/************************** Function Implementation *************************/
#ifndef __linux__
int XDehaze_CfgInitialize(XDehaze *InstancePtr, XDehaze_Config *ConfigPtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(ConfigPtr != NULL);

    InstancePtr->Ctrl_BaseAddress = ConfigPtr->Ctrl_BaseAddress;
    InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

    return XST_SUCCESS;
}
#endif

void XDehaze_Start(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL) & 0x80;
    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL, Data | 0x01);
}

u32 XDehaze_IsDone(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL);
    return (Data >> 1) & 0x1;
}

u32 XDehaze_IsIdle(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL);
    return (Data >> 2) & 0x1;
}

u32 XDehaze_IsReady(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL);
    // check ap_start to see if the pcore is ready for next input
    return !(Data & 0x1);
}

void XDehaze_EnableAutoRestart(XDehaze *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL, 0x80);
}

void XDehaze_DisableAutoRestart(XDehaze *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_AP_CTRL, 0);
}

void XDehaze_Set_ctrl_reg(XDehaze *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_CTRL_REG_DATA, Data);
}

u32 XDehaze_Get_ctrl_reg(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_CTRL_REG_DATA);
    return Data;
}

void XDehaze_Set_threshold_reg(XDehaze *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_THRESHOLD_REG_DATA, Data);
}

u32 XDehaze_Get_threshold_reg(XDehaze *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_THRESHOLD_REG_DATA);
    return Data;
}

void XDehaze_InterruptGlobalEnable(XDehaze *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_GIE, 1);
}

void XDehaze_InterruptGlobalDisable(XDehaze *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_GIE, 0);
}

void XDehaze_InterruptEnable(XDehaze *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_IER);
    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_IER, Register | Mask);
}

void XDehaze_InterruptDisable(XDehaze *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_IER);
    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_IER, Register & (~Mask));
}

void XDehaze_InterruptClear(XDehaze *InstancePtr, u32 Mask) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XDehaze_WriteReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_ISR, Mask);
}

u32 XDehaze_InterruptGetEnabled(XDehaze *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_IER);
}

u32 XDehaze_InterruptGetStatus(XDehaze *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XDehaze_ReadReg(InstancePtr->Ctrl_BaseAddress, XDEHAZE_CTRL_ADDR_ISR);
}

