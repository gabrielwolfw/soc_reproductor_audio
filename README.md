# soc_reproductor_audio

### Generación hps_0.h descripción del hardware en el diseño de qsys

'''
sopc-create-header-files ../soc_system.sopcinfo --single hps_0.h --module hps_0
'''


// Aplica el ajuste de volumen
left_16 = left_16 / 4;
right_16 = right_16 / 4;

// Escribe el valor del canal izquierdo ÚNICAMENTE en el registro izquierdo.
iowrite32(left_16, audiobase + AUDIO_LEFTDATA_REG);

// Escribe el valor del canal derecho ÚNICAMENTE en el registro derecho.
iowrite32(right_16, audiobase + AUDIO_RIGHTDATA_REG);
