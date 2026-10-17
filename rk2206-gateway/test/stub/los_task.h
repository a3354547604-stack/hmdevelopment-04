/* 主机替身：los_task.h（真机来自 kernel/liteos_m/kernel/include）。顺带给出 printf：
 * SDK 样例都不显式 include stdio.h 而直接用 printf，说明真头链里已经带了；替身要一致。 */
#ifndef GW_STUB_LOS_TASK_H
#define GW_STUB_LOS_TASK_H
#include <stdint.h>
#include <stdio.h>

typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t  UINT8;
typedef char     CHAR;
typedef void *   VOID_PTR;

#define LOS_OK 0u

typedef void (*TSK_ENTRY_FUNC)(void *arg);

typedef struct {
    TSK_ENTRY_FUNC pfnTaskEntry;
    UINT16         usTaskPrio;
    UINT32         uwStackSize;
    CHAR          *pcName;
    UINT32         uwArg;
    UINT32         uwResved;
} TSK_INIT_PARAM_S;

UINT32 LOS_TaskCreate(UINT32 *taskId, TSK_INIT_PARAM_S *initParam);
void   LOS_Msleep(UINT32 ms);
#endif
