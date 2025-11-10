import sounddevice as sd
import numpy as np
import serial
import time

# --- настройки ---
COM_PORT = "COM4"
BAUD = 115200
THRESHOLD = 0.2  # минимальный уровень шума (настрой по микрофону)
SMOOTH = 0.2  # сглаживание (0–1, меньше = плавнее)
CALIBRATION_TIME = 3  # секунд автоопределения порога при запуске

# --- подключение ---
arduino = serial.Serial(COM_PORT, BAUD)
time.sleep(2)

# --- переменные ---
vu_smooth = 0
band_smooth = np.zeros(3)
noise_floor = 0.0
calibrating = True
calib_data = []


def audio_callback(indata, frames, time_info, status):
    global vu_smooth, band_smooth, noise_floor, calibrating

    mono = np.mean(indata, axis=1)
    rms = np.sqrt(np.mean(mono**2))  # реальный уровень громкости

    # во время калибровки собираем шумовой фон
    if calibrating:
        calib_data.append(rms)
        return

    # шумовой порог
    level = max(0, rms - noise_floor)
    if level < THRESHOLD:
        level = 0

    # сглаживание VU
    vu_smooth = vu_smooth * (1 - SMOOTH) + level * SMOOTH
    vu = int(np.clip(vu_smooth * 4000, 0, 1023))  # масштаб под Arduino

    # спектр (3 полосы)
    spectrum = np.abs(np.fft.rfft(mono))
    bands = np.array_split(spectrum, 3)
    band_vals = []
    for i in range(3):
        val = np.mean(bands[i])
        val = max(0, val - noise_floor * 5)  # убрать фон
        band_smooth[i] = band_smooth[i] * (1 - SMOOTH) + val * SMOOTH
        band_vals.append(int(np.clip(band_smooth[i] * 50, 0, 1023)))

    msg = f"vu:{vu};bands:{band_vals[0]},{band_vals[1]},{band_vals[2]}\n"
    print(msg.strip())
    arduino.write(msg.encode())


# --- основной блок ---
with sd.InputStream(channels=1, samplerate=44100, callback=audio_callback):
    print("Калибровка микрофона... не издавай звуков 3 секунды...")
    start_time = time.time()
    while time.time() - start_time < CALIBRATION_TIME:
        time.sleep(0.05)
    if len(calib_data) > 0:
        noise_floor = np.mean(calib_data) * 1.1  # чуть выше среднего шума
        calibrating = False
    print(f"Калибровка завершена. Порог шума: {noise_floor:.5f}")

    print("Передача звука в Arduino...")
    while True:
        time.sleep(0.001)
