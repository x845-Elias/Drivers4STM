/* ********************************************************************************
 * File: 	mlx90393.c (Code Source File)
 * Brief: 	Implementación de la Biblioteca para el magnetómetro MLX90393 via I2C
 * 			Arquitectura en tres capas:
 *				[L1] Bajo NIvel - Comandos raw I2C, parseo de bytes
 *				[L2] Medio		- conversión uT/ºC, offsets de calibración
 * 				[L3] Alto Nivel - Promediado, Lectura lista para FreeRTOS
 *
 * Target: 	STM32F429i-Discovery (STM32F429ZIT6)
 * Bus:		I2C3 - PxN (SCL) / PxN (SDA) - Configurado en CubeMx
 * Sensor: 	Melexis MLX90393 (tres ejes + temperatura)
 *
 * Dependencias:
 * 		- stm32f4xx_hal.h
 *		- MLX90393.h
 *		- FreeRTOS / cmsis_os.h	
 *
 * @author: C. Elias Fernando Mata Cruz
 * @version:V1.0.0
 * @date: 	April 2026
 * ********************************************************************************
*/

/* -- Includes -- */
#include "mlx90393.h"
#include <math.h>
#include <string.h>

/* Integración con FreeRTOS opcional:
 * Si el proyecto usa FreeRTOS, los delays internos usan 
 * osDelay () en lugar de HAL_Helay (), cediendo CPU al scheduler */
#ifdef USE_FREERTOS
	#include "cmsis_os.h"
	#define MLX_DELAY(ms) osDelay(ms)
#else 
	#define MLX_DELAY(ms) HAL_Delay(ms)
#endif

/* -- Macro: esperar I2C_IT listo (cede CPU si FreeRTOS esta Activo) -- */
#define MLX_WAIT_READY(hmlx, ret_on_fail)                               \
    do {                                                                \
        uint32_t _t = HAL_GetTick();                                    \
        while (HAL_I2C_GetState((hmlx)->hi2c) != HAL_I2C_STATE_READY)   \
        {                                                               \
            if ((HAL_GetTick() - _t) > MLX90393_TIMEOUT_MS)             \
                return (ret_on_fail);                                   \
            MLX_DELAY(1);                                               \
        }                                                               \
    } while(0)
		
/* **************************************************************************
 * [L1] Capa Baja - Comandos I2C Directos
 * ************************************************************************** */
 
/**
 * @brief Envía un byte de comando al sensor sin payload adicional.
		  Usado internamente por Reset, Init y ReadRaw para EX y RT
 * @param hmlx: Handle del sensor (hi2c debe estar inicializado)
 * @param cmd: Byte de comando (defines del MLX90393_CMD_*)
 * @retval MLX90393_OK si el HAL acepta la transmisión y el bus esta listo
		   MLX90393_ERR_I2C si HAL rechaza la operación
		   MLX90393_ERR_TIMEOUT si el bus no queda listo antes del timeout
 */
MLX90393_Status_t MLX90393_SendCmd (MLX90393_Handle_t *hmlx, uint8_t cmd)
{	// Lanzar transmión no bloqueante (IT)
	if (HAL_I2C_Master_Transmit_IT (hmlx->hi2c, MLX90393_ADDR, &cmd, 1) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	// Esperar a que el periferico termine (cede CPU en FreeRTOS)
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	
	return MLX90393_OK;
}

/**
 * @brief Ejecuta la secuencia completa de reset del MLX90393
		  Secuencia del datasheet ss15.3.1.2
		  1) EX - Salir del modo activo
		  2) RT - Reset de registros a valores OTP
		  3) Leer byte de status - confirma que el sensor respondio
		  
 * @note Tras RT el sensor requiere TPOR (0.6ms - 1.5ms); se espera 2ms
 * @param hmlx: Handle del sensor (hi2c debe estar inicializado)
 * @retval MLX90393_OK si el sensor respondio al status byte post-reset
 */
MLX90393_Status_t MLX90393_Reset (MLX90393_Handle_t *hmlx)
{
	uint8_t status= 0;
	
	// Paso 1: EX - Salir de cualquier modo
	if (MLX90393_SendCmd (hmlx, MLX90393_CMD_EX) != MLX90393_OK)
		return MLX90393_ERR_I2C;
	
	MLX_DELAY (1);
	
	// Paso 2: RT - Reset Completo
	if (MLX90393_SendCmd (hmlx, MLX90393_CMD_RT) != MLX90393_OK)
		return MLX90393_ERR_I2C;
	
	//Esperar TPOR con margen de seguridad
	MLX_DELAY (2);
	
	// Paso 3: Leer status byte de confirmación
	if (HAL_I2C_Master_Receive_IT (hmlx->hi2c, MLX90393_ADDR, &status, 1) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	 
	return MLX90393_OK;
}

/**
 * @brief Lee una medición completa XYZT del MLX90393
		  Secuencia del datasheet ss15.3.1.3
		  1) SM XYZT - inicia medición única de los cuatro canales
		  2) Leer status byte del SM - confirma aceptación
		  3) osDelay (MLX90393_TCONV_MS) - esperar conversión ADC
		  4) RM XYZT - solicitar lectura de resultados
		  5) Leer 9 bytes: status (1) + T (2) + X (2) + Y (2) + Z (2)
		  6) Parsear MSB-First (big-endian)
 *
 * @note Los valores quedan en hmlx->raw. Llamar MLX90393_ConvertToPhys ()
         despues para obtener uT/ºC
 * @note Usa HAL_I2C_..._IT para no bloquar el scheduler FreeRTOS durante
		 la espera de conversión
 * @param hmlx: Handle del sensor
 * @retval MLX90393_OK si toda la secuencia fue exitosa
		   MLX90393_ERR_I2C, MLX90393_ERR_TIMEOUT o MLX90393_ERR_STAT en error.
 */
MLX90393_Status_t MLX90393_ReadRaw (MLX90393_Handle_t *hmlx)
{
	uint8_t cmd;
	uint8_t status_sm;
	// 9 bytes: status (1) + TH + TL + XH + XL + YH + YL + ZH + ZL
	uint8_t raw [9];
	
	/* Paso 1: SM XYZT - Iniciar Medicion */
	cmd= MLX90393_CMD_SM_XYZT;
	if (HAL_I2C_Master_Transmit_IT (hmlx->hi2c, MLX90393_ADDR, &cmd, 1) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	
	/* Paso 2: Leer Status Byte del SM */
	if (HAL_I2C_Master_Receive_IT (hmlx->hi2c, MLX90393_ADDR, &status_sm, 1) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	
	// Bit4 del status= ERROR - abortar si esta activo 
	if (status_sm & 0x10)
		return MLX90393_ERR_STAT;
	
	/* Paso 3: Esperar converisón ADC (segun FreeRTOS o no) */
	MLX_DELAY (MLX90393_TCONV_MS);
	
	/* Paso 4: RM XYZT - Solicitar Resultados */
	cmd= MLX90393_CMD_RM_XYZT;
	if (HAL_I2C_Master_Transmit_IT (hmlx->hi2c, MLX90393_ADDR, &cmd, 1) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	
	/* Paso 5: Leer 9 bytes de resulado */
	if (HAL_I2C_Master_Receive_IT (hmlx->hi2c, MLX90393_ADDR, raw, 9) != HAL_OK)
		return MLX90393_ERR_I2C;
	
	MLX_WAIT_READY (hmlx, MLX90393_ERR_TIMEOUT);
	
	// Verificar bit Error en el Byte de Status
	if (raw [0] & 0x10)
		return MLX90393_ERR_STAT;
	
	/* Paso 6: Parsear - BigEndian */
	hmlx->raw.status= raw [0];								// raw[0]= Status RM
    hmlx->raw.raw_t= (uint16_t)((raw [1] << 8) | raw [2]);	// raw[1]:raw[2]= T
    hmlx->raw.raw_x= (int16_t)((raw [3] << 8) | raw [4]);	// raw[3]:raw[4]= X
    hmlx->raw.raw_y= (int16_t)((raw [5] << 8) | raw [6]);	// raw[5]:raw[6]= Y
    hmlx->raw.raw_z= (int16_t)((raw [7] << 8) | raw [8]);	// raw[7]:raw[8]= Z
	
	return MLX90393_OK;
}

/* **************************************************************************
 * [L2] Capa Media - Conversión a unidades físicas y calibración de offset
 * ************************************************************************** */
 
 /**
 * @brief Convierte los datos crudos del handle a unidades físicas. 
		  Aplica la sensibilidad del sensor y la formula de temperatura
		  del datasheet ss11. Si hay calibración activa (hmlx->cal.calibrated == 1)
		  resta los offsets estáticos. Calcula tambien la magnitud total.
 *
 * @note Modifica hmlx->phys, debe llamarse despues de MLX90393_ReadRaw():
 * @param hmlx: Handle del sensor (raw debe ser valido)
 * @retval MLX90393_OK siempre 
 */
MLX90393_Status_t MLX90393_ConvertToPhys (MLX90393_Handle_t *hmlx)
{	// Conversión de campo magnetico
	hmlx-> phys.mag_x= (float)hmlx->raw.raw_x * MLX90393_SENS_XY;
	hmlx-> phys.mag_y= (float)hmlx->raw.raw_y * MLX90393_SENS_XY;
	hmlx-> phys.mag_z= (float)hmlx->raw.raw_z * MLX90393_SENS_Z;
	
	// Aplicar Offsets de calibración
	if (hmlx->cal.calibrated)
	{
		hmlx->phys.mag_x -= hmlx->cal.offset_x;
		hmlx->phys.mag_y -= hmlx->cal.offset_y;
		hmlx->phys.mag_z -= hmlx->cal.offset_z;
	}
	
	// Conversión de temperatura
	hmlx->phys.temp= ((float)hmlx->raw.raw_t - MLX90393_TEMP_T25) / MLX90393_TEMP_RES + 25.0f;
	
	// Magnitud Total del Campo
	hmlx->phys.mag_total= sqrtf (hmlx->phys.mag_x * hmlx->phys.mag_x + hmlx->phys.mag_y * hmlx->phys.mag_y + hmlx->phys.mag_z * hmlx->phys.mag_z);
	
	return MLX90393_OK;
}

 /**
 * @brief Calibra offsets estaticos del sensor promediando lecturas en
		  un entorno de campo nulo (sin iman proximo, lejos de perturbaciones)
		  Toma MLX90393_CAL_SAMPLES lectruas crudas y promedia caada eje. 
		  El resultado se guarda en hmlx->cal y se activa calibrated= 1..
 *
 * @note Bloque furante aproximadamente CAL_SAMPLES * TCONV_MS + delays
		 Con USE_FREERTOS usa osDelay y cede CPU entre muestras
		 Sin USE_FREERTOS, bloquea el core durante la calibración
 * @note Para calibrar: Mantener el sensor quieto, sin imanes cerca. 
 * @param hmlx: Handle del sensor
 * @retval MLX90393_OK si todas las muestras son correctas
		   MLX90393_ERR_I2C / MLX90393_ERR_STAT si alguna lectura falla
 */
MLX90393_Status_t MLX90393_Calibrate (MLX90393_Handle_t *hmlx)
{
	float sum_x= 0.0f;
	float sum_y= 0.0f;
	float sum_z= 0.0f;
	MLX90393_Status_t ret;
	
	// Resetar calibración anteior al inciiar
	hmlx->cal.calibrated= 0;
	hmlx->cal.offset_x= 0.0f;
	hmlx->cal.offset_y= 0.0f;
	hmlx->cal.offset_z= 0.0f;
	
	for (uint8_t i= 0; i < MLX90393_CAL_SAMPLES; i++)
	{	// Leer muestra cruda
		ret= MLX90393_ReadRaw (hmlx);
		if (ret != MLX90393_OK)
			return ret;
		
		// COnvertir a uT sin offsets (Calibrated = 0)
		MLX90393_ConvertToPhys (hmlx);
		
		// Acumular
		sum_x += hmlx->phys.mag_x;
		sum_y += hmlx->phys.mag_y;
		sum_z += hmlx->phys.mag_z;
		
		// Pausa - cede CPU en FreeRTOS
		MLX_DELAY (5);
	}
	
	// Calcular y guardar offsets promedio 
	hmlx->cal.offset_x= sum_x / MLX90393_CAL_SAMPLES;
    hmlx->cal.offset_y= sum_y / MLX90393_CAL_SAMPLES;
    hmlx->cal.offset_z= sum_z / MLX90393_CAL_SAMPLES;
    hmlx->cal.calibrated= 1;
 
    return MLX90393_OK;
}

/* **************************************************************************
 * [L3] Capa Alta - Lectura completa con promediado deslizante
 * ************************************************************************** */
 
 /**
 * @brief Inicializa el sensor MLX90393 de forma completa
			a) Asignar hi2c al handle y limpiar buffers
			b) Verificar presencia en el bus I2C 
			c) Ejecutar secuencoa de reset
			d) Precalentar el buffer de promediado 
 *
 * @note Debe llamarse antes del scheduler FreeRTOS, o al inicio de la tarea que gestiona al sensor
 * @note Si el sensor no esta conectado (MLX90393_ERR_NC), la función retorna
		 ese codigo y setea hmlx->connected= 0. La aplicación debe seguir funcionando sin el sensor 
 * @param hmlx: Handle a inicializar
 * @param hi2c: Puntero al handle I2C de HAL generado por CubeMx
 * @retval MLX90393_OK si el sensor respondió e inicializo correctamente
		   MLX90393_ERR_NC si no hay respuesta en el bus I2C
 */
MLX90393_Status_t MLX90393_Init (MLX90393_Handle_t *hmlx, I2C_HandleTypeDef *hi2c)
{	//  Limpiar el handle completamente 
    memset (hmlx, 0, sizeof (MLX90393_Handle_t));
    hmlx->hi2c = hi2c;
 
    // Verificar presencia en el bus (2 reintentos, 10ms timeout)
    if (HAL_I2C_IsDeviceReady (hmlx->hi2c, MLX90393_ADDR, 2, 10) != HAL_OK)
    {
        hmlx->connected= 0;
        return MLX90393_ERR_NC;
    }
 
    // Reset hardware del sensor 
    if (MLX90393_Reset (hmlx) != MLX90393_OK)
    {
        hmlx->connected= 0;
        return MLX90393_ERR_I2C;
    }
 
    hmlx->connected= 1;
 
    // Precalentar el buffer de promediado
    for (uint8_t i = 0; i < MLX90393_AVG_SAMPLES; i++)
    {
        if (MLX90393_ReadRaw (hmlx) == MLX90393_OK)
        {
            MLX90393_ConvertToPhys (hmlx);
            // Llenar el buffer con el mismo valor inicial
            hmlx->_buf_x [i]= hmlx->phys.mag_x;
            hmlx->_buf_y [i]= hmlx->phys.mag_y;
            hmlx->_buf_z [i]= hmlx->phys.mag_z;
        }
        MLX_DELAY (15);
    }
 
    hmlx->_buf_idx= 0;
    hmlx->_buf_ready= 1;
 
    return MLX90393_OK;
}

/**
 * @brief Lee una muestra, la convierte a uT y actualiza la ventana
		  deslizanre de promediado. El resultado promediado queda en 
		  hmlx->phys (sobrescribe la lectura instantanea)
		  
		  Flujo:
			ReadRaw -> ConvertToPhys (lectura instantanea) -> Insertar en buffer circular
			Calcular promedio -> sobreescribir hmlx->phys con el promediado
 *
 * @note Esta es la funciñin a llamar en el loop de la tarea FreeRTO 
		 Llamar bajo mutex en FreeRTOS antes de leer hmlx->phys desde otra tarea
 * @param hmlx: Hanlde inicializado con MLX90393_Init
 * @retval MLX90393_OK si la muestra fue leida y promediada
		   MLX90393_ERR_I2C / MLX90393_ERR_STAT si falla la lectura raw
 */
MLX90393_Status_t MLX90393_ReadAveraged (MLX90393_Handle_t *hmlx)
{
    float sum_x= 0.0f;
    float sum_y= 0.0f;
    float sum_z= 0.0f;
    MLX90393_Status_t ret;
 
    // Leer muestra cruda y convertir
    ret= MLX90393_ReadRaw (hmlx);
    if (ret != MLX90393_OK)
        return ret;
 
    MLX90393_ConvertToPhys (hmlx);
 
    // Actualizar buffer circular
    hmlx->_buf_x [hmlx->_buf_idx]= hmlx->phys.mag_x;
    hmlx->_buf_y [hmlx->_buf_idx]= hmlx->phys.mag_y;
    hmlx->_buf_z [hmlx->_buf_idx]= hmlx->phys.mag_z;
 
    hmlx->_buf_idx= (hmlx->_buf_idx + 1) % MLX90393_AVG_SAMPLES;
 
    //Calcular promedio de la ventana completa
    for (uint8_t i = 0; i < MLX90393_AVG_SAMPLES; i++)
    {
        sum_x += hmlx->_buf_x [i];
        sum_y += hmlx->_buf_y [i];
        sum_z += hmlx->_buf_z [i];
    }
 
    // Sobreescribir phys con los valores promediados
    hmlx->phys.mag_x= sum_x / MLX90393_AVG_SAMPLES;
    hmlx->phys.mag_y= sum_y / MLX90393_AVG_SAMPLES;
    hmlx->phys.mag_z= sum_z / MLX90393_AVG_SAMPLES;
 
    // Recalcular magnitud con los valores promediados 
    hmlx->phys.mag_total = sqrtf (hmlx->phys.mag_x * hmlx->phys.mag_x + hmlx->phys.mag_y * hmlx->phys.mag_y + hmlx->phys.mag_z * hmlx->phys.mag_z);
 
    return MLX90393_OK;
}

 /**
 * @brief Retorna un puntero const a los datos fisicos del handle.
		  Uso seguro para leer desde la tarea de display bajo mutex
		  sin copiar toda la estructura.
 *
 * @note El puntero es valido mientras el handle exista en memoria
		 No llamar fuera del mutez en entoreno multitarea
 * @param hmlx: Hanlde del sensor
 * @retval Puntero Const a MLX90393_PhysData_t
 */
const MLX90393_PhysData_t* MLX90393_GetPhysData (const MLX90393_Handle_t *hmlx)
{
	return &hmlx->phys;
}
