/* ********************************************************************************
 * File: 	bmi160.c (Code Source File)
 * Brief: 	Implementación de la Biblioteca para la IMU BMI160 via I2C
 * 			Arquitectura en tres capas:
 *				[L1] Bajo NIvel - Acceso directo al Bus I2C (WriteReg / ReadRegs)
 *				[L2] Medio		- Conversión a º/s y g, offsets de calibración
 * 				[L3] Alto Nivel - Inicialización Completa, Calibración, Lectura
 *
 * Target: 	STM32F429i-Discovery (STM32F429ZIT6)
 * Bus:		I2Cx - PxN (SCL) / PxN (SDA) - Configurado en CubeMx
 * Sensor: 	Bosch BMI160 (acelerómetro + giroscopio, tres ejes cada uno)
 *
 * Dependencias:
 * 		- stm32f4xx_hal.h
 *		- bmi160.h
 *		- FreeRTOS / cmsis_os.h	
 *
 * @author: C. Elias Fernando Mata Cruz
 * @version:V1.0.0
 * @date: 	May 2026
 * ********************************************************************************
*/

/* -- Includes -- */
#include "bmi160.h"
#include "cmsis_os.h"

/* **************************************************************************
 * Helper Privado
 * ************************************************************************** */
 
/**
 * @brief Aplica zona muerta simétrica a un valor flotante.
 *		  Si |value| < threshold -> retorna 0.0f (ruido de cuantización en reposo).
 *		  Si |value| >= threshold -> retorna value sin modificar.
 */
static float BMI160_DeathZone (float value, float threshold)
{
	return (fabsf (value) < threshold) ? 0.0f : value;
}

/* **************************************************************************
 * [L1] Capa Baja - Acceso I2C Directo
 * ************************************************************************** */
 
/**
 * @brief Escribe un byte en un registro del BMI160.
 *		  Protocolo I2C del BMI160 (datasheet ss3.2.2): START | ADDR+W | REG | DATA | STOP
 *		  Se empaquetan reg y data en un buffer de 2 bytes y se transmiten
 *		  en un solo frame con HAL_I2C_Master_Transmit (bloqueante)
 * @param himu: Handle del sensor (se accede a himu->hi2c)
 * @param reg:  Dirección del registro destino (0x00 - 0x7F)
 * @param data: Valor a escribir
 * @retval HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT
 */
HAL_StatusTypeDef BMI160_WriteReg (BMI160_Handle_t *himu, uint8_t reg, uint8_t data)
{
	uint8_t buf [2]= {reg, data};
	return HAL_I2C_Master_Transmit (himu->hi2c, BMI160_ADDR, buf, 2, BMI160_TIMEOUT_MS);
}
 
/**
 * @brief Lee N bytes consecutivos desde un registro del BMI160 (burst read).
 *		  Protocolo de burst read (datasheet ss3.2.3):
 *		  1) START | ADDR+W | REG | STOP  <- envío de dirección de inicio
 *		  2) START | ADDR+R | DATA[0..N-1] | STOP  <- recepción de datos
 *		  El sensor auto-incrementa la dirección del registro en cada byte,
 *		  permitiendo capturar bloques contiguos en una sola transacción
 * @param himu:  Handle del sensor
 * @param reg:   Registro de inicio
 * @param pData: Buffer de recepción (debe tener al menos `size` bytes)
 * @param size:  Número de bytes a leer
 * @retval HAL_OK si ambas transacciones fueron exitosas
 * 		   HAL_ERROR / HAL_TIMEOUT en fallo
 */
HAL_StatusTypeDef BMI160_ReadRegs (BMI160_Handle_t *himu, uint8_t reg, uint8_t *pData, uint16_t size)
{
	HAL_StatusTypeDef ret;
 
	// Fase 1: Transmitir dirección del registro de inicio
	ret= HAL_I2C_Master_Transmit (himu->hi2c, BMI160_ADDR, &reg, 1, BMI160_TIMEOUT_MS);
	if (ret != HAL_OK) return ret;
 
	// Fase 2: Recibir datos (auto-incremento interno del BMI160)
	return HAL_I2C_Master_Receive (himu->hi2c, BMI160_ADDR, pData, size, BMI160_TIMEOUT_MS);
}
 
/* **************************************************************************
 * [L3] Capa Alta - Inicialización y Calibración
 * ************************************************************************** */
 
/**
 * @brief Inicializa el BMI160 con la secuencia PMU completa. Secuencia del datasheet ss4.2:
 *		  1) Soft reset (CMD 0xB6) - lleva todos los registros a valores por defecto
 *			 y coloca ACC y GYR en suspend mode. Esperar 100ms (TPOR post-reset)
 *		  2) Verificar CHIP_ID (reg 0x00) - debe responder 0xD1
 *			 Si no coincide: himu->connected= 0, retornar 0
 *		  3) Acelerómetro -> normal mode (CMD 0x11)
 *			 Tiempo de arranque desde suspend: ~3.8ms (tabla 10)
 *			 Se espera 100ms para cubrir variaciones de tolerancia
 *		  4) Giroscopio -> normal mode (CMD 0x15)
 *			 Tiempo de arranque desde suspend: ~80ms (tabla 10)
 *			 Se espera 100ms para garantizar estabilidad del oscilador
 *		  5) Configurar giroscopio: GYR_CONF= 0x28 (ODR 100Hz), GYR_RANGE= 0x02 (±500°/s)
 *		  6) Configurar acelerómetro: ACC_CONF= 0x28 (ODR 100Hz), ACC_RANGE= 0x08 (±8g)
 * @param himu: Handle a inicializar (debe estar a {0} antes de llamar)
 * @param hi2c: Puntero al handle I2C del HAL (ej. &hi2c3)
 * @retval 1 si el sensor inicializó correctamente
 *		   0 si no hubo respuesta o el CHIP_ID no coincide
 */
uint8_t BMI160_Init (BMI160_Handle_t *himu, I2C_HandleTypeDef *hi2c)
{
	uint8_t chip_id= 0;
 
	// Inicializar handle a estado limpio
	memset (himu, 0, sizeof (BMI160_Handle_t));
	himu->hi2c= hi2c;
 
	// Paso 1: Soft reset - estado conocido de todos los registros
	BMI160_WriteReg (himu, BMI160_REG_CMD, BMI160_CMD_SOFTRST);
	HAL_Delay (100); // TPOR - datasheet ss4.2
 
	// Paso 2: Verificar CHIP_ID - confirmar presencia en bus
	BMI160_ReadRegs (himu, BMI160_REG_CHIP_ID, &chip_id, 1);
	if (chip_id != BMI160_CHIP_ID)
	{
		himu->connected= 0;
		return 0;
	}
 
	// Paso 3: Acelerómetro -> normal mode
	BMI160_WriteReg (himu, BMI160_REG_CMD, BMI160_CMD_ACC_NORM);
	HAL_Delay (100); // Startup time ACC (datasheet tabla 10)
 
	// Paso 4: Giroscopio -> normal mode
	BMI160_WriteReg (himu, BMI160_REG_CMD, BMI160_CMD_GYR_NORM);
	HAL_Delay (100); // Startup time GYR ~80ms (datasheet tabla 10)
 
	// Paso 5: Configurar giroscopio - ODR 100Hz, ±500°/s
	BMI160_WriteReg (himu, BMI160_REG_GYR_CONF, BMI160_GYR_CONF_VAL);
	HAL_Delay (2);
	BMI160_WriteReg (himu, BMI160_REG_GYR_RNG, BMI160_GYR_RANGE_VAL);
	HAL_Delay (2);
 
	// Paso 6: Configurar acelerómetro - ODR 100Hz, ±8g
	BMI160_WriteReg (himu, BMI160_REG_ACC_CONF, BMI160_ACC_CONF_VAL);
	HAL_Delay (2);
	BMI160_WriteReg (himu, BMI160_REG_ACC_RNG, BMI160_ACC_RANGE_VAL);
	HAL_Delay (2);
 
	himu->connected= 1;
	return 1;
}
 
/**
 * @brief Calibra los offsets del giroscopio del BMI160.
 *		  Promedia BMI160_CAL_SAMPLES lecturas crudas en reposo.
 *		  Resultado guardado en himu->phys.offset_{x,y,z}.
 * @param himu: Handle inicializado con BMI160_Init
 */
void BMI160_CaliGyr (BMI160_Handle_t *himu)
{
	float sum_x= 0.0f, sum_y= 0.0f, sum_z= 0.0f;
 
	if (himu->connected == 0) return;
 
	for (uint16_t i= 0; i < BMI160_CAL_SAMPLES; i++)
	{
		BMI160_ReadRaw (himu);
 
		// Acumular en unidades físicas para promediar directamente en °/s
		sum_x += (float)himu->raw.raw_gyr_x * BMI160_GYR_SENS;
		sum_y += (float)himu->raw.raw_gyr_y * BMI160_GYR_SENS;
		sum_z += (float)himu->raw.raw_gyr_z * BMI160_GYR_SENS;
 
		osDelay (5); // Cede CPU - esperar nueva muestra (ODR= 100Hz)
	}
 
	himu->phys.offset_x= sum_x / (float)BMI160_CAL_SAMPLES;
	himu->phys.offset_y= sum_y / (float)BMI160_CAL_SAMPLES;
	himu->phys.offset_z= sum_z / (float)BMI160_CAL_SAMPLES;
	himu->gyr_cal= 1;
}

/* **************************************************************************
 * [L1] Capa Baja - Lectura de Datos Crudos
 * ************************************************************************** */
 
/**
 * @brief Lee el bloque completo giroscopio + acelerómetro en un burst read.
 * @note  Si la lectura I2C falla la función retorna sin modificar himu->raw,
 *		  conservando el último valor válido
 * @param himu: Handle inicializado y conectado
 */
void BMI160_ReadRaw (BMI160_Handle_t *himu)
{	// 12 bytes: GYR_X(2) + GYR_Y(2) + GYR_Z(2) + ACC_X(2) + ACC_Y(2) + ACC_Z(2)
	uint8_t buf [12];
 
	if (BMI160_ReadRegs (himu, BMI160_REG_GYR_X_L, buf, 12) != HAL_OK)
		return; // Conservar último dato válido en caso de fallo
 
	// Parseo Little-Endian - buf[N]= LSB, buf[N+1]= MSB
	himu->raw.raw_gyr_x= (int16_t)((buf [1]  << 8) | buf [0]);
	himu->raw.raw_gyr_y= (int16_t)((buf [3]  << 8) | buf [2]);
	himu->raw.raw_gyr_z= (int16_t)((buf [5]  << 8) | buf [4]);
	himu->raw.raw_acc_x= (int16_t)((buf [7]  << 8) | buf [6]);
	himu->raw.raw_acc_y= (int16_t)((buf [9]  << 8) | buf [8]);
	himu->raw.raw_acc_z= (int16_t)((buf [11] << 8) | buf [10]);
}
 
/* **************************************************************************
 * [L2] Capa Media - Conversión a Unidades Físicas
 * ************************************************************************** */
 
/**
 * @brief Convierte los datos crudos del BMI160 a unidades físicas.
 *		  Aplica la sensibilidad del sensor configurado y los offsets de calibración.
 * @param himu: Handle con himu->raw válido (llamar BMI160_ReadRaw antes)
 */
void BMI160_Convert (BMI160_Handle_t *himu)
{
	// Giroscopio: escalar -> restar offset -> zona muerta
	float gx= (float)himu->raw.raw_gyr_x * BMI160_GYR_SENS - himu->phys.offset_x;
	float gy= (float)himu->raw.raw_gyr_y * BMI160_GYR_SENS - himu->phys.offset_y;
	float gz= (float)himu->raw.raw_gyr_z * BMI160_GYR_SENS - himu->phys.offset_z;
 
	himu->phys.gyr_x= BMI160_DeathZone (gx, BMI160_DEATHZONE);
	himu->phys.gyr_y= BMI160_DeathZone (gy, BMI160_DEATHZONE);
	himu->phys.gyr_z= BMI160_DeathZone (gz, BMI160_DEATHZONE);
 
	// Acelerómetro: solo escalar (sin zona muerta - señal DC válida)
	himu->phys.acc_x= (float)himu->raw.raw_acc_x * BMI160_ACC_SENS;
	himu->phys.acc_y= (float)himu->raw.raw_acc_y * BMI160_ACC_SENS;
	himu->phys.acc_z= (float)himu->raw.raw_acc_z * BMI160_ACC_SENS;
}
