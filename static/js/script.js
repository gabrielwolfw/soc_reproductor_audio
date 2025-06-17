// Configuración
const API_BASE = window.location.origin;

// Formatear tiempo de segundos a MM:SS
function formatTime(seconds) {
    const mins = Math.floor(seconds / 60);
    const secs = Math.floor(seconds % 60);
    return `${mins}:${secs < 10 ? '0' : ''}${secs}`;
}

// Convertir tiempo MM:SS a segundos
function timeToSeconds(timeStr) {
    const [mins, secs] = timeStr.split(':').map(Number);
    return mins * 60 + secs;
}

// Actualizar interfaz con datos de la canción
function updateSongInfo(songData) {
    document.getElementById('cover').src = songData.song.cover;
    document.getElementById('song-title').textContent = songData.song.title;
    document.getElementById('song-artist').textContent = songData.song.artist;
    document.getElementById('song-album').textContent = songData.song.album;
    document.getElementById('song-year').textContent = songData.song.year;
    document.getElementById('song-genre').textContent = songData.song.genre;
    document.getElementById('song-duration').textContent = songData.song.duration;
    document.getElementById('total-time').textContent = songData.song.duration;
    
    // Actualizar tiempo actual
    const currentTime = formatTime(songData.current_time);
    document.getElementById('current-time').textContent = currentTime;
    
    // Actualizar barra de progreso
    const totalSeconds = timeToSeconds(songData.song.duration);
    const progressPercent = (songData.current_time / totalSeconds) * 100;
    document.getElementById('progress-bar').style.width = `${progressPercent}%`;
    
    // Actualizar botón de play/pause
    const playBtn = document.getElementById('play-btn');
    playBtn.innerHTML = songData.is_playing ? 
        '<i class="fas fa-pause"></i>' : '<i class="fas fa-play"></i>';
}

// Controlar el reproductor
async function controlPlayer(action) {
    try {
        const response = await fetch(`${API_BASE}/api/control/${action}`);
        const data = await response.json();
        if (data.status === 'success') {
            updateCurrentSong();
        }
    } catch (error) {
        console.error('Error controlling player:', error);
    }
}

// Obtener y actualizar canción actual
async function updateCurrentSong() {
    try {
        const response = await fetch(`${API_BASE}/api/current_song`);
        const data = await response.json();
        updateSongInfo(data);
    } catch (error) {
        console.error('Error updating song info:', error);
    }
}

// Actualizar cada segundo
setInterval(updateCurrentSong, 1000);

// Inicializar al cargar la página
document.addEventListener('DOMContentLoaded', updateCurrentSong);