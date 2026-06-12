/* **************************************************************************
 * File: 	bmi160.h (Header File)
 * Brief: 	Biblioteca para la IMU BMI160 (Bosch) via I2C
 * 			Arquitectura en tres capas:
 *				[L1] Bajo NIvel - Acceso directo al bus I2C (WriteReg / ReadReg)
 *				[L2] Medio		- Conversión a º/s y g, Offsets de Calibración
 * 				[L3] Alto Nivel - Inicialización completa, calibración, lectura
 *
 * Target: 	STM32F429i-Discovery (STM32F429ZIT6)
 * Bus:		I2Cx - PxN (SCL) / PxN (SDA) - Configurado en CubeMx
 * Sensor: 	Bosch BMI160 (Acelerómetro + Giroscopio, tres ejes cada uno)
 *
 * @author: C. Elias Fernando Mata Cruz
 * @version:V1.0.0
 * @date: 	May 2026
 * **************************************************************************
*/

#ifndef BMI160_H
#define BMI160_H

/* -- Includes -- */
#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

 
/* **************************************************************************
 * Sección 1 - Dirección I2C y Chip ID
 * ************************************************************************** */
/* Dirección base segun Pin SA0 */
// SA0 = GND -> Dirección 7-bit 0x68 -> HAL 8-bit = 0xD0 (default)
// SA0 = VDD -> Dirección 7-bit 0x69 -> HAL 8-bit = 0xD2 
// Ajustar BMI160_ADDR según la conexión de SA0 en Hardware
#define BMI160_ADDR_SA0_GND 	(0x68 << 1)			// 0xD0 - SA0 = GND
#define BMI160_ADDR_SA0_VDD 	(0x69 << 1)			// 0xD2 - SA0 = VDD
#define BMI160_ADDR 			BMI160_ADDR_SA0_GND // Selección Activa
 
/* Valor esperado en el registro CHIP_ID (0x00) para identificar BMI160 */
#define BMI160_CHIP_ID 0xD1
 
/* Timeout genérico para operaciones HAL I2C (ms) */
#define BMI160_TIMEOUT_MS 50
 
/* **************************************************************************
 * Sección 2 - Mapa de Registros (datasheet sección 2.4)
 * ************************************************************************** */
#define BMI160_REG_CHIP_ID 	0x00	// Identificación del Chip (solo lectura)
#define BMI160_REG_ERR_REG 	0x02	// Registro de errores de Sensores
#define BMI160_REG_PMU_STAT 0x03 	// Estado PMU de cada sensor (solo lectura)
#define BMI160_REG_GYR_X_L 	0x0C	// Byte bajo de GYR_X - inicio burst giroscopio
#define BMI160_REG_ACC_X_L 	0x12	// Byte bajo de ACC_X - inicio burst acelerómetro
#define BMI160_REG_STATUS	0x1B	// Estado deñ sensor
#define BMI160_REG_TEMP_L	0x20	// Temperatura Intern - byte ajo
#define BMI160_REG_ACC_CONF	0x40	// Configuración ODR/BWP del Acelerómetro 
#define BMI160_REG_ACC_RNG	0x41	// Rango FS del Acelerómetro
#define BMI160_REG_GYR_CONF 0x42	// Configuración ODR/BWP del Giroscopio
#define BMI160_REG_GYR_RNG	0x43	// Rango FS del Giroscopio
#define BMI160_REG_CMD 		0x7E	// Registro de COmando PMU y Reset

/* **************************************************************************
 * Sección 3 - Comandos PMU (registro CMD 0x7E, datasheet tala 9)
 * ************************************************************************** */
#define BMI160_CMD_ACC_SUSP	0x10	// Acelerómetro -> suspend mode
#define BMI160_CMD_ACC_NORM 0x11	// Acelerómetro -> normal mode
#define BMI160_CMD_ACC_LOW 	0x12	// Acelerómetro -> low-power mode
#define BMI160_CMD_GYR_SUSP 0x14	// Giroscopio -> suspend mode
#define BMI160_CMD_GYR_NORM 0x15	// Giroscopio -> normal mode
#define BMI160_CMD_GYR_FAST 0x17	// Giroscopio -> fast start-up mode
#define BMI160_CMD_SOFTRST	0xB6	// Soft reset completo (vuelve a suspend)

/* **************************************************************************
 * Sección 4 - Sensibilidad y Conversión 
 *			   Giroscopio [GYR_RANGE= 0x02, ±500°/s, datasheet tabla 14]
 * 			   Acelerómetro [ACC_RANGE= 0x08, ±8g, datasheet tabla 15]
 * ************************************************************************** */ 
/* Configuración del Giroscopio:
 * GYR_CONF (0x42)= 0x28 -> ODR 100 Hz. BWP Normal
 * GYR_RANGE (0x43)= 0x02 -> ±500º/s, 65.6 LSB/º/s */
#define BMI160_GYR_CONF_VAL 	0x28
#define BMI160_GYR_RANGE_VAL	0x02
#define BMI160_GYR_SENS			(1.0f / 65.6f)	// º/s por LSB a ±500º/s	

/* Configuración del Acelerómetro:
 * ACC_CONF (0x40)= 0x28 -> ODR 100 Hz. BWP Normal
 * GYR_RANGE (0x41)= 0x08 -> ±8g, 4096 LSB/g 		*/
#define BMI160_ACC_CONF_VAL 	0x28
#define BMI160_ACC_RANGE_VAL	0x08
#define BMI160_ACC_SENS			(8.0f / 32768.f) // g por LSB a ±8g

/* **************************************************************************
 * Sección 5 - Parámetros de Operación
 * ************************************************************************** */
/* Muestras de para calibración del Giroscopio */
#define BMI160_CAL_SAMPLES 	256

/* Zona Muerta del Giroscopio (º/s) */
#define BMI160_DEATHZONE		2.0f

/* **************************************************************************
 * Sección 6 - Tipos y Estructuras
 * ************************************************************************** */
 
/* -- Códigos de Retorno -- */
typedef enum 
{
	BMI160_OK= 0x00, 			//Operación Exitosa
	BMI160_ERR_I2C= 0x01, 		// Fallo de Transacción I2C (HAL Rechazo)
	BMI160_ERR_CHIPID= 0x02,	// CHIP_ID leido no coincie con el 0xD1
	BMI160_ERR_NC= 0x03			// Sensor no Conectado / Sin respuesta en el Bus
} BMI160_Status_t;

/* -- Datos Crudos (L1) -- */
typedef struct 
{
	int16_t raw_gyr_x;	// ADC Giroscopio, eje X (LSB)
	int16_t raw_gyr_y;	// ADC Giroscopio, eje Y (LSB)
	int16_t raw_gyr_z;	// ADC Giroscopio, eje Z (LSB)
	int16_t raw_acc_x;	// ADC Acelerómetro, eje X (LSB)
	int16_t raw_acc_y;	// ADC Acelerómetro, eje Y (LSB)
	int16_t raw_acc_z;	// ADC Acelerómetro, eje Z (LSB)
} BMI160_RawData_t; 

/* -- Datos Físicos (L2) -- */
typedef struct 
{
	float gyr_x; 	// Velocidad angular eje X (grados / seg)
	float gyr_y;	// Velocidad angular eje Y (grados / seg)
	float gyr_z;	// Velocidad angular eje Z (grados / seg)
	float acc_x;	// Aceleración eje X (g)
	float acc_y;	// Aceleración eje Y (g)
	float acc_z;	// Aceleración eje Z (g)
	float angle_x;	// Ángulo Integrado eje X (grados)
	float angle_y;	// Ángulo Integrado eje Y (grados)
	float angle_z;	// Ángulo Integrado eje Z (grados)
	float offset_x;	// Offset de Calibración del Giroscopio X
	float offset_y;	// Offset de Calibración del Giroscopio Y
	float offset_z;	// Offset de Calibración del Giroscopio Z
} BMI160_PhysData_t;

/* -- Handle Principal del Sensor -- */
typedef struct 
{	// Referencia al periferico I2C 
	I2C_HandleTypeDef *hi2c;
	
	// Estado de Conexión
	uint8_t connected; 	// 1= Sensor respondio al Init y CHIP_ID correcto
	uint8_t gyr_cal;	// 1= Calibración del Giroscopio completada
	
	// Datos por Capa
	BMI160_RawData_t raw;	// Última lectura cruda (L1)
	BMI160_PhysData_t phys;	// Datos en Unidades Físicas (L2)
} BMI160_Handle_t;

/* **************************************************************************
 * Sección 7 - Prototipos de Funciones
 * ************************************************************************** */
/* -- [L1] Capa Baja - Acceso I2C directo -- */
 
/**
 * @brief Escribe un byte en un registro del BMI160.
 *		  Empaqueta [reg, data] en un frame de 2 bytes y transmite con
 *		  HAL_I2C_Master_Transmit (bloqueante, timeout BMI160_TIMEOUT_MS)
 * @param himu: Handle del sensor
 * @param reg:  Dirección del registro destino (0x00 - 0x7F)
 * @param data: Valor a escribir
 * @retval HAL_OK si la transmisión fue exitosa
 * 		   HAL_ERROR / HAL_BUSY / HAL_TIMEOUT en fallo
 */
HAL_StatusTypeDef BMI160_WriteReg (BMI160_Handle_t *himu, uint8_t reg, uint8_t data);
 
/**
 * @brief Lee N bytes consecutivos desde un registro del BMI160 (burst read).
 *		  El sensor auto-incrementa la dirección en cada byte (datasheet ss3.2.3).
 *		  Secuencia:
 *		  1) Transmitir dirección del registro de inicio
 *		  2) Recibir N bytes de datos
 * @param himu:  Handle del sensor
 * @param reg:   Registro de inicio
 * @param pData: Buffer de recepción (mínimo 'size' bytes)
 * @param size:  Número de bytes a leer
 * @retval HAL_OK si ambas transacciones fueron exitosas
 * 		   HAL_ERROR / HAL_TIMEOUT en fallo
 */
HAL_StatusTypeDef BMI160_ReadRegs (BMI160_Handle_t *himu, uint8_t reg, uint8_t *pData, uint16_t size); 

/* -- [L2] Capa Media - Conversión y Calibración -- */

/**
 * @brief Convierte los datos crudos del handle a unidades físicas.
 *		  Aplica sensibilidad, offset de calibración y zona muerta al giroscopio.
 *		  Aplica solo sensibilidad al acelerómetro (sin zona muerta: señal DC válida).
 * @note  La integración de ángulos se delega a la tarea para controlar dt con precisión.
 *		  Llamar después de BMI160_ReadRaw()
 * @param himu: Handle del sensor (raw debe ser válido)
 */
void BMI160_Convert (BMI160_Handle_t *himu);

/* -- [L3] Capa Alta - Inicialización, Calibración y Lectura */

/**
 * @brief Inicializa el BMI160 con la secuencia PMU completa:
 *		  1) Soft reset (CMD 0xB6) + espera 100ms (TPOR, datasheet ss4.2)
 *		  2) Verificar CHIP_ID == 0xD1 para confirmar presencia en bus
 *		  3) Acelerómetro -> normal mode (CMD 0x11) + espera 100ms
 *		  4) Giroscopio   -> normal mode (CMD 0x15) + espera 100ms
 *		  5) Configurar ODR/BWP y rango del giroscopio
 *		  6) Configurar ODR/BWP y rango del acelerómetro
 * @note  Llamar desde la tarea IMU (scheduler ya activo), no desde MX_FREERTOS_Init.
 *		  Los HAL_Delay son bloqueantes pero solo afectan a la tarea IMU durante el arranque.
 * @param himu: Handle a inicializar (debe estar a {0})
 * @param hi2c: Puntero al handle I2C de HAL (ej. &hi2c3)
 * @retval 1 si el sensor inicializó correctamente
 *		   0 si no hubo respuesta o el CHIP_ID no coincide
 */
uint8_t BMI160_Init (BMI160_Handle_t *himu, I2C_HandleTypeDef *hi2c);

/**
 * @brief Calibra los offsets del giroscopio promediando lecturas en reposo.
 *		  Toma BMI160_CAL_SAMPLES muestras crudas y promedia cada eje.
 *		  Resultado guardado en himu->phys.offset_{x,y,z}.
 *		  Activa himu->gyr_cal= 1 al completar.
 * @note  Requiere FreeRTOS activo (usa osDelay entre muestras).
 *		  Mantener el sensor inmóvil sobre superficie plana durante la calibración.
 *		  Si himu->connected == 0 retorna sin hacer nada.
 * @param himu: Handle inicializado con BMI160_Init
 */
void BMI160_CaliGyr (BMI160_Handle_t *himu);

/**
 * @brief Lee el bloque completo giroscopio + acelerómetro en un burst read de 12 bytes.
 *		  Registros 0x0C-0x17 (GYR_X/Y/Z + ACC_X/Y/Z). Parseo little-endian (LSB primero).
 *		  Resultado disponible en himu->raw
 * @note  Si la lectura I2C falla la función retorna sin modificar himu->raw
 *		  conservando el último valor válido
 * @param himu: Handle inicializado y conectado
 */
void BMI160_ReadRaw (BMI160_Handle_t *himu);

#endif /* BMI160_H */
