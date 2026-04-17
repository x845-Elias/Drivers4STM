/* **************************************************************************
 * File: 	mlx90393.h (Header File)
 * Brief: 	Biblioteca para el magnetómetro MLX90393 via I2C
 * 			Arquitectura en tres capas:
 *				[L1] Bajo NIvel - Comandos raw I2C, parseo de bytes
 *				[L2] Medio		- conversión uT/ºC, offsets de calibración
 * 				[L3] Alto Nivel - Promediado, Lectura lista para FreeRTOS
 *
 * Target: 	STM32F429i-Discovery (STM32F429ZIT6)
 * Bus:		I2C3 - PxN (SCL) / PxN (SDA) - Configurado en CubeMx
 * Sensor: 	Melexis MLX90393 (tres ejes + temperatura)
 *
 * @author: C. Elias Fernando Mata Cruz
 * @version:V1.0.0
 * @date: 	April 2026
 * **************************************************************************
*/

#ifndef MLX90393_H
#define MLX90393_H

/* -- Includes -- */
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* **************************************************************************
 * Sección 1 - Dirección I2C y Comandos (datasheet tabla 11)
 * ************************************************************************** */
 
/* Dirección base: A0= GND, A1= GND -> 0x0C; HAL usa 8bits (shift left 1) */
#define MLX90393_ADDR	(0x0C << 1)

/* Comandos de operación */
#define MLX90393_CMD_NOP		0x00	// No Op
#define MLX90393_CMD_SB			0x10	// Start Burst (todos los ejes)
#define MLX90393_CMD_SW			0x20	// Start Wake-on-Change
#define MLX90393_CMD_SM_XYZ 	0x3E	// Single Meas: Z+Y+X
#define MLX90393_CMD_SM_XYZT	0x3F	// Single Meas: Z+Y+X+T
#define MLX90393_CMD_RM_XYZT	0x4F	// Read Meas: Z+Y+X+T
#define MLX90393_CMD_RR			0x50	// Read Register
#define MLX90393_CMD_WR			0x60	// Write Register
#define MLX90393_CMD_EX			0x80	// Exit Mode
#define MLX90393_CMD_HR			0xD0	// Memory Recall
#define MLX90393_CMD_HS			0xE0	// Memory Store
#define MLX90393_CMD_RT			0xF0	// Reset

/* **************************************************************************
 * Sección 2 - Registros Internos (AKn Map - datasheet tabla 18)
 * ************************************************************************** */
#define MLX90393_REG_0	0x00	// HALLCONF, GAIN_SEL, Z_SERIES
#define MLX90393_REG_1	0x01	// BIST, Z_SERIES, XYZT_RES
#define MLX90393_REG_2	0x02	// DIG_FILT, OSR, TRIG_INT_SEL
#define MLX90393_REG_3	0x03	// SENS_TC_LT, SENS_TC_HT

/* **************************************************************************
 * Sección 3 - Sensibilidad y Conversión (datasheet tabla 17)
 *			   [GAIN_SEL= 7, RES= 0, HALLCONF= 0x0C]
 * ************************************************************************** */
#define MLX90393_SENS_XY	0.150f		// uT / LSB - ejes X e Y
#define MLX90393_SENS_Z		0.242f		// uT / LSB - eje Z
#define MLX90393_TEMP_T25	46244.0f	// LSB a 25ºC (datasheet ss11)
#define MLX90393_TEMP_RES	45.2f		// LSB / ºC

/* **************************************************************************
 * Sección 4 - Oarámetros de Operación
 * ************************************************************************** */
 
/* Tiempo de Conversión (ms) - OSR=0, DIG_FILT= 0, TCONV= 5ms */
#define MLX90393_TCONV_MS			10

/* Timeout para Operaciones I2c */
#define MLX90393_TIMEOUT_MS			50

/* Tamaño del buffer de promediado (Capa Alta) */
#define MLX90393_AVG_SAMPLES		8

/* Umbral Minimo de campo para considerar medición valida (uT) */
#define MLX90393_FIELD_THRESHOLD	5.0f

/* Muestras para calibración de offset */
#define MLX90393_CAL_SAMPLES		64

/* **************************************************************************
 * Sección 5 - Tipos y Estructuras
 * ************************************************************************** */
 
/* -- Códigos de Retorno -- */
typedef enum
{
	MLX90393_OK= 0x00,			// Operación Exitosa
	MLX90393_ERR_I2C= 0x01,		// Fallo de transacción I2C
	MLX90393_ERR_STAT= 0x02,	// Sensor reportó error en byte de status
	MLX90393_ERR_TIMEOUT= 0x03,	// Timeout esperado HAL_I2C_STATE_READY
	MLX90393_ERR_NC= 0x04		// Sensor no conectado (HAL_I2C_IsDeviceReady)
} MLX90393_Status_t;

/* -- Datos Crudos -- */
typedef struct
{
	int16_t raw_x;	// Valor ADC eje X (LSB)
	int16_t raw_y;	// Valor ADC eje Y (LSB)
	int16_t raw_z;	// Valor ADC eje Z (LSB)
	uint16_t raw_t;	// Valor ADC temperatura (LSB)
	uint8_t status;	// Byte Status de la última transacción RM
} MLX90393_RawData_t;

/* -- Datos Físicos (Capa Media) -- */
typedef struct
{
	float mag_x;		// Campo Magnetico eje X (uT)
	float mag_y;		// Campo Magnetico eje Y (uT)
	float mag_z;		// Campo Magnetico eje Z (uT)
	float temp;			// Temperatura interna del sensor (ºC)
	float mag_total;	// Magnitud total |B|= sqrt(x^2 + y^2 + z^2) (uT)
} MLX90393_PhysData_t;

/* -- Offsets de Calibración (Capa Media) -- */
typedef struct
{
	float offset_x;		// Offest eje X
	float offset_y;		// Offset eje Y
	float offset_z;		// Offset eje Z
	uint8_t calibrated;	// 1= Calibración completa
} MLX90393_CalData_t;

/* -- Handle principal del sensor -- */

/* Agrupa todos los datos y el puntero al periferico I2C */
typedef struct
{
	/* Referencia al periferico I2C */
	I2C_HandleTypeDef *hi2c;
	
	/* Estado de conexion */
	uint8_t connected; // 1= sensor respondio al init
	
	/* Datos por capa */
	MLX90393_RawData_t raw;		// Última lectura cruda
	MLX90393_PhysData_t phys;	// Última lectura en unidades fisica
	MLX90393_CalData_t cal;		// Offsets de calibración
	
	/* Buffer interno de promediado (capa alta) */
	float _buf_x [MLX90393_AVG_SAMPLES];
	float _buf_y [MLX90393_AVG_SAMPLES];
	float _buf_z [MLX90393_AVG_SAMPLES];
	uint8_t _buf_idx;	// Indice circular del buffer
	uint8_t _buf_ready; // 1= buffer precalentado
} MLX90393_Handle_t;

/* **************************************************************************
 * Sección 5 - Tipos y Estructuras
 * ************************************************************************** */
 
/* -- [L1] Capa Baja - Comandos I2C directos -- */

/**
 * @brief Envía un byte de comando al sensor (sin payload)
 * @param hmlx: Handle del sensor
 * @param cmd: Comando
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_SendCmd (MLX90393_Handle_t *hmlx, uint8_t cmd);

/**
 * @brief Ejecuta la secuencia de reset hardware (EX -> RT -> Leer Status) 
 * @param hmlx: Handle del sensor
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_Reset (MLX90393_Handle_t *hmlx);

/**
 * @brief Lee una medición cruda (SM -> wait -> RM -> parseo)
		  Usa HAL_I2C_..._IT para no bloquear el scheduler FreeRTOS
 * @param hmlx: Handle del sensor
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_ReadRaw (MLX90393_Handle_t *hmlx);

/* -- [L2] Capa Media - Conversión y Calibración -- */

/**
 * @brief Convierte datos crudos del handle en unidades fisicas (uT/ºC)
          y calcula la magnitud. Aplica offsets si hay calibración activa
 * @param hmlx: Handle del sensor (raw valido)
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_ConvertToPhys (MLX90393_Handle_t *hmlx);

/**
 * @brief Calibra los offsets estaticos promediado MLX90393_CAL_SAMPLES
		  lecturas con el sensor en reposo (campo nulo)
		  Bloquea durante la calibración
		  Compatible con FreeRTOS: usa osDelay internamente si se define	
		  el simbollo USE_FREERTOS
 * @param hmlx: Handle del sensor
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_Calibrate (MLX90393_Handle_t *hmlx);

/* -- [L3] Capa Alta - Lectura complea con promediado -- */

/**
 * @brief Inicializa el sensor: verifica presencia I2C, hace reset y
		  precalienta el buffer de promediado.
		  Debe llamarse una vez anres del loop principal o de la tarea FreeRTOS
 * @param hmlx: Handle a inicializar
 * @param hi2c: Puntero al Handle I2C de HAL
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_Init (MLX90393_Handle_t *hmlx, I2C_HandleTypeDef *hi2c);

/**
 * @brief Lee una muestra, convierte a fisicos y actualiza el promedio
          deslizante. Resultado promediado disponible en hmlx->phys.
		  Función de uso normal en el Loop de la tarea FreeRTOS.
 * @param hmlx: Handle del sensor (debe haber pasado por MLX90393_Init)
 * @retval MLX90393_Status_t
 */
MLX90393_Status_t MLX90393_ReadAveraged (MLX90393_Handle_t *hmlx);

/**
 * @brief Obtiene el puntero de los datos físicos promediados del handle
		  Útil para leer los datos desde otra tarea (mutex)
 * @param hmlx: Handle del sensor
 * @retval Puntero a MLX90393_PhysData_t (no copiar, proteger con Mutex)
 */
const MLX90393_PhysData_t* MLX90393_GetPhysData (const MLX90393_Handle_t *hmlx);

#endif /* MLX90393_H */
