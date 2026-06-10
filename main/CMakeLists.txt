if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# ===== Edge Impulse: modelo de gestos (parte de IA) =====
# ei-model/ = export "C++ library" do Edge Impulse Studio (SDK + modelo
# treinado). Compilamos o SDK inteiro junto do firmware.
set(EI_MODEL_DIR ${CMAKE_CURRENT_LIST_DIR}/../ei-model)
set(EI_SDK_DIR ${EI_MODEL_DIR}/edge-impulse-sdk)
include(${EI_SDK_DIR}/cmake/utils.cmake)

RECURSIVE_FIND_FILE(EI_SRC_CPP "${EI_SDK_DIR}" "*.cpp")
RECURSIVE_FIND_FILE(EI_SRC_CC  "${EI_SDK_DIR}" "*.cc")
RECURSIVE_FIND_FILE(EI_SRC_C   "${EI_SDK_DIR}" "*.c")
RECURSIVE_FIND_FILE(EI_SRC_S   "${EI_SDK_DIR}" "*.s")
RECURSIVE_FIND_FILE(EI_SRC_MODEL "${EI_MODEL_DIR}/tflite-model" "*.cpp")

add_executable(pico_emb
    main.c
    imu_ei.cpp
    hc06.c
    protocol.c
    ssd1306/ssd1306.c
    ${EI_SRC_CPP}
    ${EI_SRC_CC}
    ${EI_SRC_C}
    ${EI_SRC_S}
    ${EI_SRC_MODEL}
    )

target_include_directories(pico_emb PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ssd1306
    ${EI_MODEL_DIR}
    ${EI_MODEL_DIR}/tflite-model
    ${EI_MODEL_DIR}/model-parameters
    ${EI_SDK_DIR}
    ${EI_SDK_DIR}/third_party/ruy
    ${EI_SDK_DIR}/third_party/gemmlowp
    ${EI_SDK_DIR}/third_party/flatbuffers/include
    ${EI_SDK_DIR}/third_party
    ${EI_SDK_DIR}/tensorflow
    ${EI_SDK_DIR}/dsp
    ${EI_SDK_DIR}/classifier
    ${EI_SDK_DIR}/anomaly
    ${EI_SDK_DIR}/CMSIS/NN/Include
    ${EI_SDK_DIR}/CMSIS/DSP/PrivateInclude
    ${EI_SDK_DIR}/CMSIS/DSP/Include
    ${EI_SDK_DIR}/CMSIS/Core/Include
    ${EI_SDK_DIR}/porting
    )

target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    Fusion
    pico_stdlib
    hardware_adc
    hardware_i2c
    hardware_irq
    hardware_pwm
    hardware_uart
    )

target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=1
    # Edge Impulse: aloca pelo heap do FreeRTOS e usa vTaskDelay no ei_sleep
    FREERTOS_ENABLED=1
    EIDSP_QUANTIZE_FILTERBANK=0
    ARM_MATH_LOOPUNROLL
    )

# stdio via USB nativa E via UART0 (GP0/GP1) — UART0 = console do Debug Probe.
# (o HC-06 fica na UART1, então a UART0 está livre pro console)
pico_enable_stdio_usb(pico_emb 1)
pico_enable_stdio_uart(pico_emb 1)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(pico_emb PRIVATE
        pico_cyw43_arch_none
        )
endif()
pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
