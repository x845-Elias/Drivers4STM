==========================================================================
	MLX90393 - Driver / Biblioteca para STM32F429i-Discovery
	Archivo: readme.txt
	Autor:	 C. Elias Fernando Mata Cruz
	Versión: V1.0.0
	Fecha:	 April 2026
==========================================================================

Índice
------
	1. Descripción general
	2. Archivos de la biblioteca
	3. Arquitectura de capas
	4. Configuración STM32CubeMx
	5. Integración del Proyecto
	6. Integración con FreeRTOS
	7. Manejo de Errores
	8. Parámetros configurables
	9. Conexión física del sensor
	
==========================================================================
		1. Descripción General 
==========================================================================

Esta biblioteca permite usar el magnetómetro Melexis MLX90393 desde un 
STM32F429i-Discovery a través del periférico I2C, empleando las funciones 
HAL generadas por STM32CubeMx.

El sensor MLX90393 mide campo magnético en tres ejes (X, Y, Z) en microtesla (uT)
y temperatura interna en ºC. Se comunica por I2C y soporta modo de medición 
única (Single Measurement), que es el modo usado por esta biblioteca.

La biblioteca está organizada en tres capas de abstracción, similar al esquema
HAL del propio STM32, para facilitar la reutilización, el testing por capa y la 
adaptación a otros proyectos. 

Sensibilidad por defecto (GAIN_SEL= 7, RES= 0, HALLCONF= 0x0C)
	Ejes X e Y: 0.150 uT/LSB
	Eje Z: 0.242 uT/LSB
	
==========================================================================
		2. Archivos de la Biblioteca
==========================================================================	
	
	mlx9039.h - Header: Defines, Structs, Enums, Prototipos
	mlx9039.c - Source: implementación completa de las tres capas
	readme.txt - Este documento
	
Los dos archivos deben incluirse en el proyecto.
	** Proceso para añadir Biblioteca **
	
==========================================================================
		3. Arquitectura de Capas
==========================================================================	

	[L3] Capa Alta
		- MLX_Init ()			- Inicialización completa + precalentamiento
		- MLX_ReadAveraged ()	- Lectura + Conversión + Promediado
		- MLX_GetPhysData ()	- Acceso seguro a datos desde otra tarea
		
	[L2] Capa Media
		- MLX90393_ConvertToPhys ()	- LSB -> uT/ºC, aplica offsets
		- MLX90393_Calibrated ()	- Promedia N lectura para calcular offset
		
	[L1] Capa Baja
		- MLX90393_SendCmd ()	- Envía comando I2C de 1 byte
		- MLX90393_Reset ()		- Secuencia EX -> RT -> Leer Status
		- MLX90393_ReadRaw ()	- Secuencia SM -> espera -> RM -> Parseo
		
	HAL I2C (STM32CubeMx)	- HAL_I2C_Master_Transmit/Receive_IT
	
La capa alta llama a la media, y la media llama a la baja. 
En uso normal con FreeRTOS se llama solo a las funciones de la capa alta.
Las capas baja y media pueden usarse de forma independiente para diagnostico
o para construir funcionalidades personalizadas.

==========================================================================
		4. Configuración en STM32CubeMx & Configuración 
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
	
	-- 4.3 Definir USE_FREERTOS (solo si usas) --
	
		#define USE_FREERTOS
	
	Esto activa osDelay () dentro de la biblioteca para ceder CPU al Scheduler
	durante las esperas de conversión y timeout. Sin esta definición se usa 
	HAL_Delay () (Bloqueante)
	
	-- 4.4 Agregar math.h (libm) --
	La biblioteca usa sqrtf(). Verificar que la librería matemática este habilitada
	
	-- 4.5 Incluir los archivos en el proyecto --
	Copiar mlx9039.c a la carpeta Core/Src/
	Copiar mlx9039.h a la carpeta Core/Inc/
	
	En el archivo que los use:
		#include "mlx9039.h"
		
==========================================================================
		5. Integración en el Proyecto (sin FreeRTOS)
==========================================================================
/* Declarar el Handle (global o estático en el modulo) */
MLX90393_Handle_t hmlx= {0};

int main (void)
{
	HAL_Init ();
	SystemClock_Config ();
	
	MX_I2Cx_Init ();
	
	/* Inicializar Sensor */
	if (MLX90393_Init (&hmlx, &hi2cx) != MLX90393_OK)
	{
		// Sensor no conectado - manejar error
	}
	
	while (1)
	{
		if (MLX90393_ReadAveraged (&hmlx) == MLX90393_OK)
		{
			float x= hmlx.phys.mag_x;
			float y= hmlx.phys.mag_y;
			float z= hmlx.phys.mag_z;
			float t= hmlx.phys.temp;
		}
		HAL_Delay (100);
	}
}	

==========================================================================
		6. Integración con FreeRTOS
==========================================================================
	
	-- 6.1 Recursos a crear en STM32CubeMx --
	Task & Queues:
	Task: MagRead_Task (o tu nombre de preferencia)
		- Task Name: MagRead_Task
		- Priority: osPriorityNormal
		- Stack Size: 384
		- Entry Function: Start_MagRead_Task
		
	Mutexes:
		- Mutex Name: MagMutex
		(protege el acceso compartido a hmlx.phys entre MagRead_Task y
		cualquier otra tarea que lea los datos)
	
	Importante: La creación del Mutex y Task se hace CubeMx para que 
	quede correctamente registrado en el código generado. No crear el mutex
	directamente como código manual
	
	-- 6.2 Variables globales requeridos --
	/* MLX90393_Handle_t HallHandle= {0}; 	// Handle del Sensor
	
	-- 6.3 Implementación de la Tarea --
	
void Start_MagRead_Task (void const *argument)
{
	/* Inicializar sensor al inicio de la tarea */
	MLX90393_Init (&HallHandle, &hi2cx)
	
	/* Si no se conecto la tarea vive en Idle sin transacciones I2C */
	if (HallHandle.connected == 0)
		for (;;) osDelay (500);
		
	for (;;)
	{
		/* Leer + promediar fuera del mutex (operación ~10ms con I2C_IT) */
		MLX90393_Status_t ret= MLX90393_ReadAveraged (&HallHandle);
		
		/* Publicar resultado bajo mutex */
		osMutexWait (MagMutexHandle, osWaitForever);
		if (ret != MLX90393_OK)
		{
			// Marcar error en alguna variable de estado si se desea 
		}
		
		/* HallHandle.phys ya fue actualizado por ReadAveraged */
		osMutexRelease (MagMutexHandle);
		
		/* ~10Hz: Read Averaged consume ~10ms, completamos a 100ms */
		osDelay (90);
	}
}
	
	-- 6.4 Leer datos en otra tarea (ej. Display) --
	
		osMutexWait(MagMutexHandle, osWaitForever);
		const MLX90393_PhysData_t *mag = MLX90393_GetPhysData(&HallHandle);
		float mx= mag->mag_x;
		float my= mag->mag_y;
		float mz= mag->mag_z;
		float mt= mag->temp;
		float mb= mag->mag_total;
		osMutexRelease(MagMutexHandle);
		

==========================================================================
		7. Manejo de Errores
==========================================================================

Todos los errores de la biblioteca se representan como MLX90393_Status_t:
	MLX90393_OK				(0x00) - Operación Exitosa
	MLX90393_ERR_I2C		(0x01) - HAL rechazo la transacción
	MLX90393_ERR_STAT		(0x02) - Sensore repoprto Error (Byte 4)
	MLX90393_ERR_TIMEOUT	(0x03) - BUs no quedo listo 	
	MLX90393_ERR_NC			(0x04) - Sensor no detectado en HAL_I2C_IsDeviceReady
	
Recomendación si la tarea falla FreeRTOS:
	- Si Init retora MLX90393_ERR_NC: sensor no conectado, no intentar más
	  transacciones, marcar hmlx.connected= 0 para que el display lo muestre.
	- Si ReadAveraged retorna error puntual: incrementar contador de errores.
	  No resetear automáticamente; si los errores persisten mas de N cilos
	  consecutivos, considerar llamar a MLX90393_Reset () y reintenter.
	
==========================================================================
		Fin del Reaadme
==========================================================================