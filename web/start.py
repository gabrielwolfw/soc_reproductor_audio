#!/usr/bin/env python3
"""
Script para iniciar ambas aplicaciones simultáneamente:
- Flask app (app.py) 
- Serial receiver (receiver.py)
"""

import subprocess
import sys
import os
import time
import signal
import threading
from pathlib import Path

class AppLauncher:
    def __init__(self):
        self.processes = []
        self.running = True
        
    def check_sudo(self):
        """Verificar si se necesitan permisos sudo para el puerto serial"""
        if os.geteuid() != 0:
            print("⚠️  Ejecutando sin sudo. Si hay problemas con el puerto serial, ejecuta:")
            print("   sudo python3 start.py")
            print()
    
    def check_files(self):
        """Verificar que los archivos necesarios existan"""
        required_files = ['app.py', 'receiver.py']  # Cambia receiver.py por tu nombre real
        missing_files = []
        
        for file in required_files:
            if not Path(file).exists():
                missing_files.append(file)
        
        if missing_files:
            print(f"❌ Archivos faltantes: {', '.join(missing_files)}")
            print("📋 Archivos necesarios:")
            print("   - app.py (Flask web server)")
            print("   - receiver.py (Serial monitor)")  # Cambia por tu nombre
            return False
        return True
    
    def start_flask_app(self):
        """Iniciar la aplicación Flask"""
        print("🌐 Iniciando Flask app...")
        try:
            process = subprocess.Popen([
                sys.executable, 'app.py'
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            self.processes.append(('Flask App', process))
            
            # Monitorear output en thread separado
            def monitor_flask():
                for line in process.stdout:
                    if self.running:
                        print(f"[FLASK] {line.strip()}")
            
            threading.Thread(target=monitor_flask, daemon=True).start()
            
            return process
            
        except Exception as e:
            print(f"❌ Error iniciando Flask: {e}")
            return None
    
    def start_serial_receiver(self):
        """Iniciar el monitor serial"""
        print("📡 Iniciando Serial receiver...")
        try:
            # Si necesitas sudo específicamente para serial
            if os.geteuid() == 0:  # Running as root
                process = subprocess.Popen([
                    sys.executable, 'receiver.py'  # Cambia por tu nombre real
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            else:
                process = subprocess.Popen([
                    sys.executable, 'receiver.py'  # Cambia por tu nombre real
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            self.processes.append(('Serial Receiver', process))
            
            # Monitorear output en thread separado
            def monitor_serial():
                for line in process.stdout:
                    if self.running:
                        print(f"[SERIAL] {line.strip()}")
            
            threading.Thread(target=monitor_serial, daemon=True).start()
            
            return process
            
        except Exception as e:
            print(f"❌ Error iniciando Serial receiver: {e}")
            return None
    
    def signal_handler(self, signum, frame):
        """Manejar Ctrl+C para cerrar todo limpiamente"""
        print("\n🛑 Cerrando aplicaciones...")
        self.running = False
        self.stop_all()
        sys.exit(0)
    
    def stop_all(self):
        """Terminar todos los procesos"""
        for name, process in self.processes:
            try:
                print(f"⏹️  Terminando {name}...")
                process.terminate()
                
                # Esperar un poco y forzar si es necesario
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    print(f"🔨 Forzando cierre de {name}...")
                    process.kill()
                    
            except Exception as e:
                print(f"⚠️  Error cerrando {name}: {e}")
    
    def wait_for_flask(self):
        """Esperar a que Flask esté listo"""
        print("⏳ Esperando que Flask esté listo...")
        max_attempts = 10
        for i in range(max_attempts):
            try:
                import requests
                response = requests.get('http://localhost:5000', timeout=1)
                if response.status_code == 200:
                    print("✅ Flask está listo!")
                    return True
            except:
                pass
            time.sleep(1)
        
        print("⚠️  Flask no respondió en tiempo esperado")
        return False
    
    def run(self):
        """Ejecutar el launcher principal"""
        print("=" * 60)
        print("🚀 LAUNCHER - Iniciador de aplicaciones")
        print("=" * 60)
        
        # Verificaciones iniciales
        self.check_sudo()
        if not self.check_files():
            return
        
        # Configurar manejo de señales
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        
        try:
            # Iniciar Flask primero
            flask_process = self.start_flask_app()
            if not flask_process:
                return
            
            # Esperar un poco para que Flask se inicie
            time.sleep(2)
            
            # Verificar que Flask esté funcionando
            if not self.wait_for_flask():
                print("❌ Flask no se inició correctamente")
                self.stop_all()
                return
            
            # Iniciar serial receiver
            serial_process = self.start_serial_receiver()
            if not serial_process:
                print("⚠️  Serial receiver no se inició, pero Flask sigue corriendo")
            
            print("\n" + "=" * 60)
            print("✅ SISTEMA INICIADO CORRECTAMENTE")
            print("🌐 Flask Web Server: http://localhost:5000")
            print("📡 Serial Monitor: Escuchando /dev/ttyUSB0")
            print("🛑 Presiona Ctrl+C para terminar")
            print("=" * 60)
            
            # Mantener el programa corriendo
            while self.running:
                time.sleep(1)
                
                # Verificar que los procesos sigan vivos
                for name, process in self.processes:
                    if process.poll() is not None:
                        print(f"⚠️  {name} terminó inesperadamente")
                        if not self.running:
                            break
                            
        except Exception as e:
            print(f"💥 Error inesperado: {e}")
        finally:
            self.stop_all()

def main():
    """Función principal"""
    launcher = AppLauncher()
    launcher.run()

if __name__ == "__main__":
    main()