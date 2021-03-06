// ESP Radiation Meter
// Author: Sergey Avdeev
// E-Mail: avdeevsv91@gmail.com
// URL: https://github.com/avdeevsv91/esp_radiation_meter

#define PUMPING_PIN    4   // Номер пина генератора накачки
#define PUMPING_PULSE  50  // Длина импульса накачки
#define PUMPING_FREQ   5   // Частота генератора накачки
#define PUMPING_ADC    450 // Максимальное напряжение делителя АЦП
#define PUMPING_POWER  400 // Напряжение питания счетчика

#define SENSOR_PIN    5  // Номер пина, к которому подсоединен датчик
#define SENSOR_TYPE   1  // Тип датчика:
                         //   СБМ-20/СТС-5/BOI-33 = 1
						 //   СБМ-19/СТС-6 = 2
#define SENSOR_NUM    1  // Количество установленных датчиков (параллельное подключение)
#define SENSOR_TIME   36 // Время для замера в секундах
#define SENSOR_SUM    10 // Количество замеров для усреднения

#define round(x) ((x)>=0?(long)((x)+0.5):(long)((x)-0.5))

// Инициализация переменных
uint16_t pumping_voltage;
uint16_t counter_value;
uint16_t counter_data[SENSOR_SUM];
uint16_t counter_result;
uint8_t counter_accuracy;
uint16_t result_counter;

// Накачка СБМ-20
void ICACHE_FLASH_ATTR pumping_function() {
	uint16_t i = 0;
	pumping_voltage = (analogRead() + 1) / (1024 / PUMPING_ADC);
	if(pumping_voltage < PUMPING_POWER) {
		digitalWrite(PUMPING_PIN, 1);
		for(i=0;i<PUMPING_PULSE;i++) {
			asm("nop");
		}
		digitalWrite(PUMPING_PIN, 0);
	}
}

// Подсчет импульсов с датчика
void counter_function() {
	counter_value++;
    // Сбрасываем состояние прерывания
    uint8_t gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(SENSOR_PIN));
}

// Старт модуля
void ICACHE_FLASH_ATTR startfunc() {
	// UART
	uart_init(BIT_RATE_9600);
	char uart_payload[32];
	uint8_t uart_len = os_sprintf(uart_payload, "\n\r");
	uart0_tx_buffer(uart_payload, uart_len);
	// Обнуляем значения
	counter_value = 0;
	counter_result = 0;
	counter_accuracy = 0;
	result_counter = 0;
	// Таймер накачки датчика
	GPIO_OUTPUT_SET(PUMPING_PIN, 0);
	static os_timer_t pumping_timer;
	os_timer_disarm(&pumping_timer);
	os_timer_setfn(&pumping_timer, (os_timer_func_t *)pumping_function, NULL);
	os_timer_arm(&pumping_timer, PUMPING_FREQ, 1);
	// Настройка прерываний для подсчета импульсов с датчика
	ETS_GPIO_INTR_DISABLE();
	ETS_GPIO_INTR_ATTACH(counter_function, NULL);
	GPIO_DIS_OUTPUT(GPIO_ID_PIN(SENSOR_PIN));
	GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(SENSOR_PIN));
	gpio_pin_intr_state_set(GPIO_ID_PIN(SENSOR_PIN), 1);
	ETS_GPIO_INTR_ENABLE();
}

uint32_t ICACHE_FLASH_ATTR calculate_urh(float cps) {
	// Линейная аппроксимация на основе графика из статьи http://sxem.org/2-vse-stati/24-izmereniya/213-radiometr-dozimetr
	// https://www.wolframalpha.com/input/?i=linear+fit+%7B200,7200%7D,%7B400,15000%7D
	#if SENSOR_TYPE == 1 // СБМ-20/СТС-5/BOI-33
		if(cps < 200) return round(cps * (36 / SENSOR_NUM)); // 36
		else if(cps >= 200 && cps < 400) return round(cps * ((39 * cps - 600) / SENSOR_NUM)); // 37.5
		else if(cps >= 400 && cps < 800) return round(cps * ((56.25 * cps - 7500) / SENSOR_NUM)); // 46.875
		else if(cps >= 800 && cps < 1400) return round(cps * ((66.6667 * cps - 15833.3) / SENSOR_NUM)); // 55.357
		else if(cps >= 1400 && cps < 2000) return round(cps * ((87.5 * cps - 45000) / SENSOR_NUM)); // 65
		else return 130000; // Предел измерений
	#elif SENSOR_TYPE == 2 // СБМ-19/СТС-6
		if(cps < 200) return round(cps * (9 / SENSOR_NUM)); // 9
		else if(cps >= 200 && cps < 400) return round(cps * ((9.75 * cps - 150) / SENSOR_NUM)); // 9.375
		else if(cps >= 400 && cps < 800) return round(cps * ((14.0625 * cps - 1875) / SENSOR_NUM)); // 11.719
		else if(cps >= 800 && cps < 1400) return round(cps * ((16.6667 * cps - 3958.33) / SENSOR_NUM)); // 13.839
		else if(cps >= 1400 && cps < 2000) return round(cps * ((21.875 * cps - 11250) / SENSOR_NUM)); // 16.25
		else return 130000; // Предел измерений
	#else // Неизвестный датчик
		return 0;
	#endif
}

// Ежесекундный таймер
void ICACHE_FLASH_ATTR timerfunc(uint32_t timersrc) {
	uint16_t i = 0;
	valdes[2] = pumping_voltage;
	if(timersrc % SENSOR_TIME == 0) { // Закончили замер
		result_counter++;
		// Сдвигаем предыдушие результаты влево
		for(i=0;i<SENSOR_SUM;i++) {
			if(i < SENSOR_SUM-1) {
				counter_data[i] = counter_data[i + 1];
			} else {
				counter_data[i] = 0;
			}
		}
		// Добавляем результаты замера в массив
		float count_per_second = (float) (counter_value / 2) / SENSOR_TIME;
		counter_data[SENSOR_SUM - 1] = calculate_urh(count_per_second);
		counter_value = 0;
		// Рассчитываем усредненное значение
		uint16_t counter_summ = 0;
		for(i=0;i<SENSOR_SUM;i++) {
			counter_summ = counter_summ + counter_data[i];
		}
		if(result_counter > SENSOR_SUM) {
			counter_result = round(counter_summ / SENSOR_SUM);
			counter_accuracy = 100;
		} else {
			counter_result = round(counter_summ / result_counter);
			counter_accuracy = round(100 * result_counter / SENSOR_SUM);
		}
		valdes[0] = counter_result;
		valdes[1] = counter_accuracy;
		// Отправка данных по UART
		char uart_payload[32];
		uint8_t uart_len = os_sprintf(uart_payload, "%d uR/h:%d%%\n\r", counter_result, counter_accuracy);
		uart0_tx_buffer(uart_payload, uart_len);
	}
}

// Вывод данных в WEB интерфейсе
void webfunc(char *pbuf) {
	os_sprintf(HTTPBUFF, "Напряжение датчика: %d V", pumping_voltage);
	os_sprintf(HTTPBUFF, "<br>Количество импульсов: %d", counter_value / 2);
	if(result_counter > 0) {
		os_sprintf(HTTPBUFF, "<br>Радиационный фон: %d мкР/ч", counter_result);
	} else {
		os_sprintf(HTTPBUFF, "<br>Радиационный фон: N/A");
	}
	os_sprintf(HTTPBUFF, "<br>Точность измерений: %d%%", counter_accuracy);
}
