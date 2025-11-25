#pragma once

// 初始化 MCU 端的“磁盘介质”（RAM Disk）
void storage_mountMCU();

// 启动 USB MSC，把 RAM Disk 暴露给 PC
void storage_mountPC();

// 在 USB 模式循环中调用，用于检测是否可以导入 SSH Key
void storage_task();
