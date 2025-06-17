from flask import Flask, render_template, jsonify
import json
import os

app = Flask(__name__)

# Configuración de rutas
DATA_DIR = os.path.join(os.path.dirname(__file__), 'pruebas')
SONGS_FILE = os.path.join(DATA_DIR, 'songs.json')
CURRENT_FILE = os.path.join(DATA_DIR, 'current.json')

@app.route('/')
def index():
    try:
        with open(SONGS_FILE, 'r', encoding='utf-8') as f:
            songs = json.load(f)
        return render_template('index.html', song=songs['songs'][0])
    except Exception as e:
        return f"Error loading songs: {str(e)}", 500

@app.route('/api/current_song')
def get_current_song():
    try:
        with open(CURRENT_FILE, 'r', encoding='utf-8') as f:
            current_data = json.load(f)
        return jsonify(current_data)
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/control/<action>')
def control_player(action):
    try:
        # Leer estado actual
        with open(CURRENT_FILE, 'r', encoding='utf-8') as f:
            current_data = json.load(f)
        
        # Leer lista de canciones
        with open(SONGS_FILE, 'r', encoding='utf-8') as f:
            songs_data = json.load(f)
        
        # Procesar acción
        if action == 'play_pause':
            current_data['is_playing'] = not current_data['is_playing']
        elif action == 'next':
            current_index = next((i for i, song in enumerate(songs_data['songs']) 
                               if song['title'] == current_data['song']['title']), 0)
            next_index = (current_index + 1) % len(songs_data['songs'])
            current_data['song'] = songs_data['songs'][next_index]
            current_data['current_time'] = 0
        elif action == 'prev':
            current_index = next((i for i, song in enumerate(songs_data['songs']) 
                               if song['title'] == current_data['song']['title']), 0)
            prev_index = (current_index - 1) % len(songs_data['songs'])
            current_data['song'] = songs_data['songs'][prev_index]
            current_data['current_time'] = 0
        
        # Guardar cambios
        with open(CURRENT_FILE, 'w', encoding='utf-8') as f:
            json.dump(current_data, f, ensure_ascii=False, indent=2)
        
        return jsonify({'status': 'success'})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == '__main__':
    # Verificar que existan los archivos JSON
    if not os.path.exists(SONGS_FILE):
        print(f"Error: No se encontró {SONGS_FILE}")
    if not os.path.exists(CURRENT_FILE):
        print(f"Error: No se encontró {CURRENT_FILE}")
    
    app.run(host='0.0.0.0', port=5000, debug=True)