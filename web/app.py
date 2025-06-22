from flask import Flask, render_template, jsonify
import json
import os
import time

app = Flask(__name__)

SONGS_FILE = os.path.join(os.path.dirname(__file__), 'pruebas', 'songs.json')

# Estado global (persistente solo mientras el servidor corre)
current_index = 0
current_time = 0
is_playing = False
last_update_time = time.time()

def get_songs():
    with open(SONGS_FILE, 'r', encoding='utf-8') as f:
        data = json.load(f)
    return data['songs']

def duration_to_seconds(duration_str):
    """Convierte MM:SS a segundos"""
    parts = duration_str.split(':')
    return int(parts[0]) * 60 + int(parts[1])

def update_current_time():
    """Actualiza el tiempo actual basado en el tiempo transcurrido"""
    global current_time, last_update_time
    
    if is_playing:
        elapsed = time.time() - last_update_time
        current_time += int(elapsed)
        
        # Verificar si la canción terminó
        songs = get_songs()
        total_seconds = duration_to_seconds(songs[current_index]['duration'])
        if current_time >= total_seconds:
            current_time = total_seconds
    
    last_update_time = time.time()

@app.route('/')
def index():
    songs = get_songs()
    return render_template('index.html', song=songs[current_index])

@app.route('/api/current_song')
def get_current_song():
    update_current_time()  # Actualizar tiempo antes de enviar
    songs = get_songs()
    return jsonify({
        "song": songs[current_index],
        "current_time": current_time,
        "is_playing": is_playing
    })

@app.route('/api/control/<action>')
def control_player(action):
    print(f"Recibido control: {action}")
    
    global current_index, current_time, is_playing, last_update_time
    
    # Actualizar tiempo actual antes de procesar comando
    update_current_time()
    
    songs = get_songs()
    
    if action == 'play_pause':
        is_playing = not is_playing
    elif action == 'play':
        is_playing = True
    elif action == 'pause':
        is_playing = False
    elif action == 'next':
        current_index = (current_index + 1) % len(songs)
        current_time = 0
    elif action == 'prev':
        current_index = (current_index - 1) % len(songs)
        current_time = 0
    
    return jsonify({
        "status": "success",
        "is_playing": is_playing,
        "current_index": current_index,
        "current_time": current_time,
        "song": songs[current_index]  # Incluir datos de canción para mejor sync
    })

@app.route('/api/seek/<int:seconds>')
def seek(seconds):
    global current_time, last_update_time
    
    songs = get_songs()
    total_seconds = duration_to_seconds(songs[current_index]['duration'])
    
    if 0 <= seconds <= total_seconds:
        current_time = seconds
        last_update_time = time.time()  # Reset del tiempo de referencia
    
    return jsonify({
        "status": "success",
        "current_time": current_time
    })

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)