const API_BASE = window.location.origin;

let currentSongData = null;
let localCurrentTime = 0;
let localIsPlaying = false;
let lastSyncTime = Date.now();

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
    const wasPlaying = localIsPlaying;
    const oldIndex = currentSongData?.song?.title;
    const newIndex = songData.song.title;
    
    currentSongData = songData;
    localCurrentTime = songData.current_time;
    localIsPlaying = songData.is_playing;
    lastSyncTime = Date.now(); // Reset del tiempo de sincronización

    // Si cambió la canción, resetear tiempo
    if (oldIndex !== newIndex) {
        localCurrentTime = 0;
    }

    document.getElementById('cover').src = songData.song.cover;
    document.getElementById('song-title').textContent = songData.song.title;
    document.getElementById('song-artist').textContent = songData.song.artist;
    document.getElementById('song-album').textContent = songData.song.album;
    document.getElementById('song-year').textContent = songData.song.year;
    document.getElementById('song-genre').textContent = songData.song.genre;
    document.getElementById('song-duration').textContent = songData.song.duration;
    document.getElementById('total-time').textContent = songData.song.duration;

    // Actualizar tiempo actual
    updateTimeDisplay();

    // Actualizar botón de play/pause
    const playBtn = document.getElementById('play-btn');
    playBtn.innerHTML = songData.is_playing ?
        '<i class="fas fa-pause"></i>' : '<i class="fas fa-play"></i>';
}

// Función separada para actualizar solo el tiempo y progreso
function updateTimeDisplay() {
    if (!currentSongData) return;
    
    document.getElementById('current-time').textContent = formatTime(localCurrentTime);
    
    const totalSeconds = timeToSeconds(currentSongData.song.duration);
    const progressPercent = Math.min((localCurrentTime / totalSeconds) * 100, 100);
    document.getElementById('progress-bar').style.width = `${progressPercent}%`;
}

// Controlar el reproductor
async function controlPlayer(action) {
    try {
        const response = await fetch(`${API_BASE}/api/control/${action}`);
        const data = await response.json();
        if (data.status === 'success') {
            // Sincronización inmediata después de control
            await updateCurrentSong();
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

// Simulación local del tiempo (más suave)
setInterval(() => {
    if (localIsPlaying && currentSongData) {
        localCurrentTime++;
        const totalSeconds = timeToSeconds(currentSongData.song.duration);
        
        // Si llegamos al final, parar
        if (localCurrentTime >= totalSeconds) {
            localCurrentTime = totalSeconds;
            localIsPlaying = false;
            // Opcional: pasar a siguiente canción automáticamente
            // controlPlayer('next');
        }
        
        updateTimeDisplay();
    }
}, 1000);

// Sincronización periódica con el servidor (cada 3 segundos)
setInterval(async () => {
    await updateCurrentSong();
}, 3000);

// Sincronización más frecuente si ha pasado mucho tiempo sin actividad
setInterval(() => {
    const timeSinceLastSync = Date.now() - lastSyncTime;
    // Si han pasado más de 10 segundos sin sincronizar, forzar sync
    if (timeSinceLastSync > 10000) {
        updateCurrentSong();
    }
}, 5000);

// Inicializar al cargar la página
document.addEventListener('DOMContentLoaded', updateCurrentSong);