#ifndef IMU_EI_H
#define IMU_EI_H

// ============================================================================
// Task de IMU + inferência Edge Impulse (parte de IA do projeto).
// Implementada em imu_ei.cpp (C++ — o SDK do Edge Impulse é C++), exposta
// aqui com linkage C para o main.c criar a task.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void imu_task(void *p);

#ifdef __cplusplus
}
#endif

#endif // IMU_EI_H
