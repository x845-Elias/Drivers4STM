==========================================================================
	BMI160 - Driver / Biblioteca para STM32F429i-Discovery
	Archivo: readme.txt
	Autor:	 C. Elias Fernando Mata Cruz
	Versión: V1.0.0
	Fecha:	 Mayo 2026
==========================================================================

Índice
------
	1. Descripción general
	2. Archivos de la biblioteca
	3. Arquitectura de capas
	4. Configuración STM32CubeMx
	5. Integración en el Proyecto (sin FreeRTOS)
	6. Integración con FreeRTOS
	7. Manejo de Errores
	8. Parámetros Configurables
	
==========================================================================
		1. Descripción General 
==========================================================================

Esta biblioteca permite usar la IMU Bosch BMI160 desde un STM32F429i-Discovery
a través del periférico I2C, empleando las funciones HAL generadas por STM32CubeMx.

El sensor BMI160 integra un giroscopio de 3 ejes y un acelerómetro de 3 ejes
en un solo encapsulado. Se comunica por I2C utilizando un modelo de Power Management
Unit (PMU) donde cada sensor debe encenderse de forma explícita antes de leer datos.

Configuración por defecto:
	Giroscopio:   ODR 100Hz, rango ±500°/s, sensibilidad 65.6 LSB/°/s
	Acelerómetro: ODR 100Hz, rango ±8g,     sensibilidad 4096 LSB/g
	
==========================================================================
		2. Archivos de la Biblioteca
==========================================================================

	bmi160.h   - Header: Defines, Structs, Enums, Prototipos
	bmi160.c   - Source: Implementación completa de las tres capas
	readme.txt - Este documento

Los dos archivos deben incluirse en el proyecto.
	
	** Proceso para añadir la Biblioteca **

==========================================================================
		3. Arquitectura de Capas
==========================================================================

	[L3] Capa Alta
		- BMI160_Init ()		- Inicialización completa con secuencia PMU
		- BMI160_CaliGyr ()		- Calibración de offset del giroscopio
		
	[L2] Capa Media
		- BMI160_Convert ()		- LSB -> °/s y g, aplica offsets y zona muerta
		
	[L1] Capa Baja
		- BMI160_ReadRaw ()		- Burst read de 12 bytes (GYR+ACC)
		- BMI160_WriteReg ()	- Escritura de 1 byte en un registro
		- BMI160_ReadRegs ()	- Lectura de N bytes consecutivos
	
	HAL I2C (STM32CubeMx)	- HAL_I2C_Master_Transmit / Receive (bloqueante)

La capa alta llama a la media, y la media llama a la baja.
En uso normal con FreeRTOS se llama principalmente a las funciones de la capa alta.
Las capas baja y media pueden usarse de forma independiente para diagnóstico
o para construir funcionalidades personalizadas.

==========================================================================
		4. Configuración en STM32CubeMx
==========================================================================

	-- 4.1 Habilitar I2C --
	Categories -> Conectivity -> I2Cx 
	Mode: I2C
		- I2C Speed Mode: Standard Mode 
		- I2C Clock Speed: 100000 Hz
		
	Los pines se asignan automaticamente a: 
	 Px# - I2Cx_SCL
	 Px# - I2Cx_SDA
	 
	-- 4.2 Habilitar Interrupciones I2Cx (para IT) --
	NVIC Settings:
		I2Cx Event Interrupt [x]
		I2Cx Error Interrupt [x]

	-- 4.3 Agregar math.h (libm) --
	La biblioteca usa fabsf(). Verificar que la librería matemática
	esté habilitada en las opciones de compilación del proyecto.

==========================================================================
		5. Integración en el Proyecto (sin FreeRTOS)
==========================================================================

/* Declarar el Handle (global o estático en el módulo) */
BMI160_Handle_t ImuHandle= {0};

int main (void)
{
	HAL_Init ();
	SystemClock_Config ();
	
	MX_I2Cx_Init ();
	
	/* Inicializar sensor */
	if (BMI160_Init (&ImuHandle, &hi2cx) == 0)
	{
		// Sensor no conectado - manejar error
	}
	
	/* Calibrar giroscopio (sensor en reposo) */
	/* Nota: sin FreeRTOS BMI160_CaliGyr usa HAL_Delay internamente	*/
	/* Modificar osDelay por HAL_Delay en bmi160.c si no hay RTOS		*/
	
	const float dt= 0.01f; // 10ms -> 100Hz
	
	while (1)
	{
		BMI160_ReadRaw (&ImuHandle);
		BMI160_Convert (&ImuHandle);
		
		/* Integrar ángulos manualmente */
		ImuHandle.phys.angle_x += ImuHandle.phys.gyr_x * dt;
		ImuHandle.phys.angle_y += ImuHandle.phys.gyr_y * dt;
		ImuHandle.phys.angle_z += ImuHandle.phys.gyr_z * dt;
		
		float gx= ImuHandle.phys.gyr_x;  // °/s
		float ax= ImuHandle.phys.acc_x;  // g
		
		HAL_Delay (10);
	}
}

==========================================================================
		6. Integración con FreeRTOS
==========================================================================

	-- 6.1 Variables globales requeridas --
	BMI160_Handle_t ImuHandle= {0};		// Handle del sensor

	-- 6.2 Implementación de la Tarea --

void Start_IMU_Task (void const *argument)
{
	const float dt= 0.01f;   // 10ms -> 100Hz (igual que el osDelay al final)

	/* Inicializar sensor al inicio de la tarea */
	if (BMI160_Init (&ImuHandle, &hi2c3) == 0)
	{	/* Sensor no detectado - tarea vive en Idle sin transacciones I2C */
		for (;;) osDelay (500);
	}

	/* Calibrar giroscopio con sensor en reposo (~1.28 segundos) */
	BMI160_CaliGyr (&ImuHandle);

	for (;;)
	{	/* Leer registros crudos - fuera del mutex (operación rapida) */
		BMI160_ReadRaw (&ImuHandle);

		/* Convertir y publicar bajo mutex */
		osMutexWait (BMI160MutexHandle, osWaitForever);

			BMI160_Convert (&ImuHandle);

			/* Integrar ángulos con dt controlado por el tick del RTOS */
			ImuHandle.phys.angle_x += ImuHandle.phys.gyr_x * dt;
			ImuHandle.phys.angle_y += ImuHandle.phys.gyr_y * dt;
			ImuHandle.phys.angle_z += ImuHandle.phys.gyr_z * dt;

			/* Límite de integración ±180° */
			if (ImuHandle.phys.angle_x >  180.0f) ImuHandle.phys.angle_x =  180.0f;
			if (ImuHandle.phys.angle_x < -180.0f) ImuHandle.phys.angle_x = -180.0f;
			if (ImuHandle.phys.angle_y >  180.0f) ImuHandle.phys.angle_y =  180.0f;
			if (ImuHandle.phys.angle_y < -180.0f) ImuHandle.phys.angle_y = -180.0f;
			if (ImuHandle.phys.angle_z >  180.0f) ImuHandle.phys.angle_z =  180.0f;
			if (ImuHandle.phys.angle_z < -180.0f) ImuHandle.phys.angle_z = -180.0f;

		osMutexRelease (BMI160MutexHandle);

		/* ~100Hz: coincidir con el dt usado en la integración */
		osDelay (10);
	}
}

	-- 6.4 Leer datos en otra tarea (ej. Display o Compare) --

		osMutexWait (BMI160MutexHandle, osWaitForever);
			float bmi_ax= ImuHandle.phys.angle_x;
			float bmi_ay= ImuHandle.phys.angle_y;
			float bmi_az= ImuHandle.phys.angle_z;
			float bmi_gx= ImuHandle.phys.gyr_x;
			float bmi_gy= ImuHandle.phys.gyr_y;
			float bmi_gz= ImuHandle.phys.gyr_z;
			float bmi_Ax= ImuHandle.phys.acc_x;
			float bmi_Ay= ImuHandle.phys.acc_y;
			float bmi_Az= ImuHandle.phys.acc_z;
			uint8_t bmi_conn= ImuHandle.connected;
		osMutexRelease (BMI160MutexHandle);

==========================================================================
		7. Manejo de Errores
==========================================================================

BMI160_Init retorna uint8_t (1= éxito, 0= fallo) en lugar de un enum de
estado, ya que la inicialización es un resultado binario: el sensor está
o no está. El campo ImuHandle.connected queda en 1 o 0 según el resultado.

BMI160_ReadRaw y BMI160_WriteReg retornan HAL_StatusTypeDef directamente,
útil para diagnóstico en capas bajas.

Recomendación si la tarea falla en FreeRTOS:
	- Si BMI160_Init retorna 0: sensor no conectado, no intentar más
	  transacciones, usar el patrón for(;;) osDelay(500) para idle.
	  ImuHandle.connected= 0 permite que el display muestre el estado.
	- Si BMI160_ReadRaw falla puntualmente: el último dato válido se
	  conserva en ImuHandle.raw. No es necesario resetear el sensor
	  por fallos aislados.
	- Si los fallos son persistentes: verificar hardware (pull-ups,
	  dirección SA0, pines CSB/SDO) antes de reintentar BMI160_Init.

==========================================================================
		8. Parámetros Configurables (bmi160.h)
==========================================================================

	BMI160_ADDR
	───────────
	Dirección I2C activa. Cambiar entre BMI160_ADDR_SA0_GND (0xD0) y
	BMI160_ADDR_SA0_VDD (0xD2) según la conexión del pin SA0.

	BMI160_TIMEOUT_MS		(default: 50ms)
	─────────────────────────────────────────
	Timeout para cada operación HAL I2C. Aumentar si el bus tiene alta
	carga por compartirse con otros sensores (ej. MLX90393).

	BMI160_GYR_RANGE_VAL + BMI160_GYR_SENS
	───────────────────────────────────────
	Rango y sensibilidad del giroscopio. Cambiar ambos juntos:
		0x00 -> ±2000°/s  |  BMI160_GYR_SENS= (1.0f / 16.4f)
		0x01 -> ±1000°/s  |  BMI160_GYR_SENS= (1.0f / 32.8f)
		0x02 ->  ±500°/s  |  BMI160_GYR_SENS= (1.0f / 65.6f)  <- activo
		0x03 ->  ±250°/s  |  BMI160_GYR_SENS= (1.0f / 131.2f)
		0x04 ->  ±125°/s  |  BMI160_GYR_SENS= (1.0f / 262.4f)

	BMI160_ACC_RANGE_VAL + BMI160_ACC_SENS
	───────────────────────────────────────
	Rango y sensibilidad del acelerómetro. Cambiar ambos juntos:
		0x03 ->  ±2g  |  BMI160_ACC_SENS= (2.0f  / 32768.0f)
		0x05 ->  ±4g  |  BMI160_ACC_SENS= (4.0f  / 32768.0f)
		0x08 ->  ±8g  |  BMI160_ACC_SENS= (8.0f  / 32768.0f)  <- activo
		0x0C -> ±16g  |  BMI160_ACC_SENS= (16.0f / 32768.0f)

	BMI160_CAL_SAMPLES		(default: 256)
	──────────────────────────────────────
	Muestras para la calibración del giroscopio.
	Duración aproximada: 256 * 5ms = 1.28 segundos.

	BMI160_DEATHZONE		(default: 2.0f °/s)
	─────────────────────────────────────────────
	Umbral de zona muerta del giroscopio. Velocidades angulares menores
	se fuerzan a 0 para eliminar deriva en reposo.
	Aumentar si el sensor muestra ruido > 2°/s en condiciones de vibración.

==========================================================================
		Fin del Readme
==========================================================================