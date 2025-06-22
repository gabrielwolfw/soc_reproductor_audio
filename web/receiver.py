import serial
import requests
import time

def open_serial():
    while True:
        try:
            return serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
        except serial.SerialException as e:
            print(f"Error puerto serial: {e}. Reintentando...")
            time.sleep(2)

ser = open_serial()
print("Monitor serial iniciado")

while True:
    try:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue
            
        command = line.lower()
        
        if command in ["play", "pause"]:
            requests.get('http://localhost:5000/api/control/play_pause')
            print(f"Comando: {command}")
        elif command == "next":
            requests.get('http://localhost:5000/api/control/next')
            print("Comando: next")
        elif command == "prev":
            requests.get('http://localhost:5000/api/control/prev')
            print("Comando: prev")
            
    except serial.SerialException:
        ser.close()
        ser = open_serial()
    except KeyboardInterrupt:
        ser.close()
        break