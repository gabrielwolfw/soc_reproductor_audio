
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Hybrid Dual AXI Audio Player - 48kHz I2S 24-bit");

/* Virtual address bases */
void *lwbridgebase;  /* Para controles (botones, display, WM8731) */
void *audiobase;     /* Para avalon_audio_slave (AXI Master) */

/* Audio player state */
typedef enum {
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_STOPPED
} audio_state_t;

static audio_state_t current_state = AUDIO_STOPPED;
static int current_track = 0;
static const int total_tracks = 3;

/* WAV file structure */
typedef struct {
    struct file *file;
    char filename[256];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t data_start_offset;
    uint32_t samples_played;
    uint32_t total_samples;
    uint8_t is_valid;
    loff_t current_pos;
} wav_file_t;

static wav_file_t songs[3];
static const char* song_paths[] = {
    "/home/root/media/sd/songs/song1.wav",
    "/home/root/media/sd/songs/song2.wav", 
    "/home/root/media/sd/songs/song3.wav"
};

/* Time tracking */
static int current_time_seconds = 0;
static int current_time_minutes = 0;
static struct timer_list audio_timer;
static struct timer_list display_timer;

/* Button definitions */
#define BUTTON_PLAY_PAUSE   0x1
#define BUTTON_NEXT         0x2
#define BUTTON_PREV         0x4

/* LW AXI Master offsets (controles) */
#define BUTTONS_BASE_OFFSET     0x8800
#define BUTTONS_DATA_OFFSET     0x0
#define BUTTONS_INTERRUPT_MASK  0x8
#define BUTTONS_EDGE_CAPTURE    0xC
#define SEVEN_SEGMENTS_BASE_OFFSET 0x8810
#define AUDIO_CONFIG_BASE_OFFSET    0x8850

/* AXI Master offsets (solo audio data) */
#define AUDIO_FIFOSPACE_REG     0x4
#define AUDIO_LEFTDATA_REG      0x8
#define AUDIO_RIGHTDATA_REG     0xC
#define AUDIO_CONTROL_REG       0x0

/* Audio buffer - optimizado para 48kHz */
#define AUDIO_BUFFER_SIZE 32768  /* 32KB buffer */
static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
static int buffer_pos = 0;
static int buffer_size = 0;
static int buffer_needs_refill = 1;

/* Workqueue for file operations */
static struct workqueue_struct *audio_wq;
static struct work_struct load_work;
static struct work_struct refill_work;
static int next_track_to_load = 0;

/* Debouncing */
static struct timespec last_button_time[3];
#define DEBOUNCE_TIME_MS 200

/* 7-segment patterns */
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

/* Function declarations */
void audio_play(void);
void audio_pause(void);
void audio_next_track(void);
void audio_prev_track(void);
void display_time_mmss(void);
void generate_audio_samples_48khz(void);
void audio_timer_callback(unsigned long data);
void display_timer_callback(unsigned long data);
int is_button_debounced(int button_num);
void load_work_handler(struct work_struct *work);
void refill_work_handler(struct work_struct *work);
int parse_wav_header_48khz(wav_file_t* wav);
void init_wm8731_via_lw_axi(void);
void init_audio_ip_via_axi(void);
void reset_audio_completely(void);

/* WM8731 I2C register addresses */
#define WM8731_LEFT_LINE_IN     0x00
#define WM8731_RIGHT_LINE_IN    0x01
#define WM8731_LEFT_HP_OUT      0x02
#define WM8731_RIGHT_HP_OUT     0x03
#define WM8731_ANALOG_PATH      0x04
#define WM8731_DIGITAL_PATH     0x05
#define WM8731_POWER_DOWN       0x06
#define WM8731_DIGITAL_IF       0x07
#define WM8731_SAMPLING_CTRL    0x08
#define WM8731_ACTIVE_CTRL      0x09
#define WM8731_RESET            0x0F

/* WM8731 initialization via LW AXI Master - CORREGIDO para I2S 24-bit */
void init_wm8731_via_lw_axi(void)
{
    printk(KERN_INFO "Initializing WM8731 for 48kHz I2S 24-bit\n");
    
    /* Reset WM8731 */
    iowrite32((WM8731_RESET << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(10);
    
    /* Power management - power up everything except mic */
    iowrite32((WM8731_POWER_DOWN << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(5);
    
    /* Left headphone volume - 0 dB */
    iowrite32((WM8731_LEFT_HP_OUT << 9) | 0x79, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Right headphone volume - 0 dB */
    iowrite32((WM8731_RIGHT_HP_OUT << 9) | 0x79, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Analog audio path - DAC selected, output muted inicialmente */
    iowrite32((WM8731_ANALOG_PATH << 9) | 0x12, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Digital audio path - disable soft mute */
    iowrite32((WM8731_DIGITAL_PATH << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Digital audio interface - I2S format, 24-bit */
    iowrite32((WM8731_DIGITAL_IF << 9) | 0x0A, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Sampling control - 48kHz, normal mode */
    iowrite32((WM8731_SAMPLING_CTRL << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    /* Activate digital audio interface */
    iowrite32((WM8731_ACTIVE_CTRL << 9) | 0x01, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(5);
    
    /* Unmute analog path */
    iowrite32((WM8731_ANALOG_PATH << 9) | 0x10, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    
    printk(KERN_INFO "WM8731 configured: I2S 24-bit, 48kHz, unmuted\n");
}

/* Complete audio reset */
void reset_audio_completely(void)
{
    printk(KERN_INFO "Complete audio reset\n");
    
    /* Reset audio IP via AXI Master */
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(10);
    iowrite32(0x2, audiobase + AUDIO_CONTROL_REG);  /* Full reset */
    mdelay(10);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(5);
}

/* Audio IP initialization via AXI Master */
void init_audio_ip_via_axi(void)
{
    uint32_t fifospace;
    
    printk(KERN_INFO "Initializing Audio IP for 48kHz I2S streaming\n");
    
    /* Complete reset first */
    reset_audio_completely();
    
    /* Enable */
    iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
    mdelay(5);
    
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
    printk(KERN_INFO "AXI Audio FIFO: 0x%08x\n", fifospace);
    
    if (fifospace == 0 || fifospace == 0xFFFFFFFF) {
        printk(KERN_WARNING "AXI FIFO not responding, extended reset\n");
        iowrite32(0x3, audiobase + AUDIO_CONTROL_REG);
        mdelay(10);
        iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
        mdelay(5);
    }
    
    printk(KERN_INFO "AXI Audio IP ready for 48kHz I2S\n");
}

/* WAV parser optimizado */
int parse_wav_header_48khz(wav_file_t* wav)
{
    uint8_t chunk_header[8];
    uint8_t fmt_chunk[16];
    mm_segment_t oldfs;
    int bytes_read;
    uint32_t chunk_size;
    loff_t current_offset;
    int found_fmt = 0;
    int found_data = 0;
    
    if (!wav->file) {
        printk(KERN_ERR "No file handle for WAV parsing\n");
        return 0;
    }
    
    printk(KERN_INFO "Parsing WAV file\n");
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    /* Read RIFF header */
    current_offset = 0;
    bytes_read = vfs_read(wav->file, chunk_header, 8, &current_offset);
    if (bytes_read != 8) {
        printk(KERN_ERR "Failed to read RIFF header\n");
        set_fs(oldfs);
        return 0;
    }
    
    /* Verify RIFF */
    if (memcmp(chunk_header, "RIFF", 4) != 0) {
        printk(KERN_ERR "Invalid RIFF header\n");
        set_fs(oldfs);
        return 0;
    }
    
    /* Read WAVE format */
    bytes_read = vfs_read(wav->file, chunk_header, 4, &current_offset);
    if (bytes_read != 4 || memcmp(chunk_header, "WAVE", 4) != 0) {
        printk(KERN_ERR "Invalid WAVE header\n");
        set_fs(oldfs);
        return 0;
    }
    
    /* Search for fmt and data chunks */
    while (current_offset < 200 && (!found_fmt || !found_data)) {
        bytes_read = vfs_read(wav->file, chunk_header, 8, &current_offset);
        if (bytes_read != 8) {
            printk(KERN_WARNING "End of header at offset %lld\n", current_offset);
            break;
        }
        
        chunk_size = *(uint32_t*)(chunk_header + 4);
        
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size >= 16) {
                bytes_read = vfs_read(wav->file, fmt_chunk, 16, &current_offset);
                if (bytes_read == 16) {
                    wav->channels = *(uint16_t*)(fmt_chunk + 2);
                    wav->sample_rate = *(uint32_t*)(fmt_chunk + 4);
                    wav->bits_per_sample = *(uint16_t*)(fmt_chunk + 14);
                    found_fmt = 1;
                    
                    printk(KERN_INFO "WAV Format: %uHz, %uch, %ubit\n", 
                           wav->sample_rate, wav->channels, wav->bits_per_sample);
                }
                if (chunk_size > 16) {
                    current_offset += (chunk_size - 16);
                }
            } else {
                current_offset += chunk_size;
            }
            
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            wav->data_size = chunk_size;
            wav->data_start_offset = current_offset;
            found_data = 1;
            
            printk(KERN_INFO "Data chunk: %u bytes at offset %u\n", 
                   wav->data_size, wav->data_start_offset);
            break;
            
        } else {
            current_offset += chunk_size;
        }
        
        if (chunk_size % 2) {
            current_offset++;
        }
    }
    
    set_fs(oldfs);
    
    if (!found_fmt || !found_data) {
        printk(KERN_ERR "Missing WAV chunks: fmt=%d data=%d\n", found_fmt, found_data);
        return 0;
    }
    
    /* Calculate total samples */
    if (wav->channels > 0 && wav->bits_per_sample > 0) {
        uint32_t bytes_per_sample = wav->channels * (wav->bits_per_sample / 8);
        wav->total_samples = wav->data_size / bytes_per_sample;
    } else {
        wav->total_samples = 0;
    }
    
    wav->samples_played = 0;
    wav->current_pos = wav->data_start_offset;
    
    printk(KERN_INFO "WAV parsed: %u samples total\n", wav->total_samples);
    
    return 1;
}

/* Load work handler */
void load_work_handler(struct work_struct *work)
{
    wav_file_t *wav;
    
    if (next_track_to_load < 0 || next_track_to_load >= total_tracks) 
        return;
    
    wav = &songs[next_track_to_load];
    
    printk(KERN_INFO "Loading track %d\n", next_track_to_load);
    
    if (wav->file) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
    }
    
    wav->file = filp_open(song_paths[next_track_to_load], O_RDONLY, 0);
    if (IS_ERR(wav->file)) {
        printk(KERN_ERR "Cannot open %s (error %ld)\n", 
               song_paths[next_track_to_load], PTR_ERR(wav->file));
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    strcpy(wav->filename, song_paths[next_track_to_load]);
    
    if (!parse_wav_header_48khz(wav)) {
        printk(KERN_ERR "Failed to parse WAV header\n");
        filp_close(wav->file, NULL);
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    wav->is_valid = 1;
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    printk(KERN_INFO "Track %d loaded successfully\n", next_track_to_load);
}

/* Refill work handler */
void refill_work_handler(struct work_struct *work)
{
    wav_file_t *wav = &songs[current_track];
    mm_segment_t oldfs;
    int bytes_read;
    
    if (!wav->is_valid || !wav->file || !buffer_needs_refill) 
        return;
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    bytes_read = vfs_read(wav->file, audio_buffer, AUDIO_BUFFER_SIZE, &wav->current_pos);
    
    set_fs(oldfs);
    
    if (bytes_read > 0) {
        buffer_size = bytes_read;
        buffer_pos = 0;
        buffer_needs_refill = 0;
    } else {
        printk(KERN_INFO "End of track reached\n");
        buffer_size = 0;
        buffer_pos = 0;
    }
}

/* Display time */
void display_time_mmss(void)
{
    uint32_t display_value;
    uint8_t min_tens, min_ones, sec_tens, sec_ones;
    
    min_tens = (current_time_minutes / 10) % 10;
    min_ones = current_time_minutes % 10;
    sec_tens = (current_time_seconds / 10) % 10;
    sec_ones = current_time_seconds % 10;
    
    display_value = ((uint32_t)seven_seg_patterns[min_tens] << 21) |
                    ((uint32_t)seven_seg_patterns[min_ones] << 14) |
                    ((uint32_t)seven_seg_patterns[sec_tens] << 7)  |
                    ((uint32_t)seven_seg_patterns[sec_ones]);
    
    iowrite32(display_value, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
}

/* CORREGIDO: Conversión de 16-bit a 24-bit I2S */
void generate_audio_samples_48khz(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right;
    int i;
    wav_file_t *wav;
    static int debug_counter = 0;
    
    if (current_state != AUDIO_PLAYING) return;
    
    wav = &songs[current_track];
    if (!wav->is_valid) {
        return;
    }
    
    /* Read FIFO space */
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
    write_space_left = (fifospace >> 24) & 0xFF;
    write_space_right = (fifospace >> 16) & 0xFF;
    
    if (write_space_left < 4 || write_space_right < 4) {
        return;
    }
    
    /* Check buffer refill */
    if (buffer_pos >= buffer_size - 8 && !buffer_needs_refill) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        return;
    }
    
    if (buffer_size == 0) {
        return;
    }
    
    /* Process samples - CORREGIDO para I2S 24-bit */
    for (i = 0; i < 8 && write_space_left > 0 && write_space_right > 0; i++) {
        int16_t left_16 = 0, right_16 = 0;
        int32_t left_24, right_24;

        if (buffer_pos >= buffer_size) {
            break;
        } else if (buffer_pos + 3 < buffer_size) {
            if (wav->bits_per_sample == 16 && wav->channels == 2) {
                /* 16-bit stereo */
                left_16  = *(int16_t*)(&audio_buffer[buffer_pos]);
                right_16 = *(int16_t*)(&audio_buffer[buffer_pos + 2]);
                buffer_pos += 4;
                wav->samples_played++;
            } else if (wav->bits_per_sample == 16 && wav->channels == 1) {
                /* 16-bit mono */
                left_16 = *(int16_t*)(&audio_buffer[buffer_pos]);
                right_16 = left_16;
                buffer_pos += 2;
                wav->samples_played++;
            } else {
                buffer_pos += 2;
                continue;
            }
        } else {
            break;
        }

        /* CONVERSIÓN CORRECTA: 16-bit a 24-bit I2S 
         * Para I2S de 24 bits, los datos se alinean en el MSB
         * Shift left 8 bits para convertir de 16-bit a 24-bit */
        left_24  = ((int32_t)left_16) << 8;
        right_24 = ((int32_t)right_16) << 8;

        /* Write to FIFO */
        iowrite32(left_24, audiobase + AUDIO_LEFTDATA_REG);
        iowrite32(right_24, audiobase + AUDIO_RIGHTDATA_REG);

        write_space_left--;
        write_space_right--;
        debug_counter++;
    }
    
    /* Log progress occasionally */
    if (debug_counter % 2000 == 0) {
        printk(KERN_INFO "Audio: samples=%u/%u pos=%d/%d\n", 
               wav->samples_played, wav->total_samples, buffer_pos, buffer_size);
    }
}

void audio_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        generate_audio_samples_48khz();
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));  /* 10ms for 48kHz */
    }
}

void display_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        current_time_seconds++;
        if (current_time_seconds >= 60) {
            current_time_seconds = 0;
            current_time_minutes++;
            if (current_time_minutes >= 100) {
                current_time_minutes = 0;
            }
        }
        display_time_mmss();
    }
    mod_timer(&display_timer, jiffies + HZ);
}

/* Debouncing */
int is_button_debounced(int button_num)
{
    struct timespec current_time;
    long diff_ms;
    
    getnstimeofday(&current_time);
    
    diff_ms = (current_time.tv_sec - last_button_time[button_num].tv_sec) * 1000 +
              (current_time.tv_nsec - last_button_time[button_num].tv_nsec) / 1000000;
    
    if (diff_ms > DEBOUNCE_TIME_MS) {
        last_button_time[button_num] = current_time;
        return 1;
    }
    return 0;
}

/* IRQ Handler */
irq_handler_t button_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    uint32_t edge_capture;
    
    edge_capture = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    
    printk(KERN_INFO "Button IRQ: 0x%x\n", edge_capture);
    
    if ((edge_capture & BUTTON_PLAY_PAUSE) && is_button_debounced(0)) {
        if (current_state == AUDIO_PLAYING) {
            audio_pause();
        } else {
            audio_play();
        }
    }
    
    if ((edge_capture & BUTTON_NEXT) && is_button_debounced(1)) {
        audio_next_track();
    }
    
    if ((edge_capture & BUTTON_PREV) && is_button_debounced(2)) {
        audio_prev_track();
    }
    
    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    return (irq_handler_t) IRQ_HANDLED;
}

/* Audio control functions */
void audio_play(void)
{
    printk(KERN_INFO "PLAY: Track %d\n", current_track);
    
    current_state = AUDIO_PLAYING;
    
    /* Ensure we have data */
    if (songs[current_track].is_valid && buffer_size == 0) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        mdelay(100);
    }
    
    /* Initialize audio for I2S */
    init_audio_ip_via_axi();
    
    /* Start timers */
    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));
    mod_timer(&display_timer, jiffies + HZ);
    
    display_time_mmss();
    
    printk(KERN_INFO "I2S playback started\n");
}

void audio_pause(void)
{
    printk(KERN_INFO "PAUSE\n");
    current_state = AUDIO_PAUSED;
    
    reset_audio_completely();
    
    del_timer(&audio_timer);
    del_timer(&display_timer);
}

void audio_next_track(void)
{
    int was_playing;
    
    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();
    
    current_track = (current_track + 1) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Next track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(200);
        audio_play();
    }
}

void audio_prev_track(void)
{
    int was_playing;
    
    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();
    
    current_track = (current_track - 1 + total_tracks) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Prev track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(200);
        audio_play();
    }
}

/* Module initialization */
static int __init initialize_48khz_audio_player(void)
{
    int result;
    int i;
    struct timespec current_time;
    
    printk(KERN_INFO "=== I2S 24-bit Audio Player ===\n");
    printk(KERN_INFO "48kHz Stereo WAV Support\n");
    
    /* Initialize debounce */
    getnstimeofday(&current_time);
    for (i = 0; i < 3; i++) {
        last_button_time[i] = current_time;
    }
    
    /* Create workqueue */
    audio_wq = create_singlethread_workqueue("audio_wq");
    if (!audio_wq) {
        printk(KERN_ERR "Failed to create workqueue\n");
        return -ENOMEM;
    }
    
    INIT_WORK(&load_work, load_work_handler);
    INIT_WORK(&refill_work, refill_work_handler);
    
    /* Initialize timers */
    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    
    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    
    /* Map memory regions */
    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    audiobase = ioremap_nocache(0xc0000000, 0x800000);
    
    if (!lwbridgebase || !audiobase) {
        printk(KERN_ERR "Memory mapping failed\n");
        if (lwbridgebase) iounmap(lwbridgebase);
        if (audiobase) iounmap(audiobase);
        if (audio_wq) destroy_workqueue(audio_wq);
        return -ENOMEM;
    }
    
    printk(KERN_INFO "Memory mapped: LW_AXI=0x%p AXI_AUDIO=0x%p\n", 
           lwbridgebase, audiobase);
    
    /* Initialize hardware */
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    
    /* Initialize WM8731 for I2S 24-bit */
    init_wm8731_via_lw_axi();
    
    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();
    
    // Load first 48kHz song
    next_track_to_load = 0;
    queue_work(audio_wq, &load_work);
    mdelay(200);
    
    // Setup interrupts
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler, 
                        IRQF_SHARED, "audio_48khz_irq", (void *)(button_irq_handler));
    
    if (result) {
        printk(KERN_ERR "IRQ request failed: %d\n", result);
        if (audio_wq) {
            flush_workqueue(audio_wq);
            destroy_workqueue(audio_wq);
        }
        iounmap(lwbridgebase);
        iounmap(audiobase);
        return result;
    }
    
    printk(KERN_INFO "=== 48kHz Audio Player Ready (Left-Justified) ===\n");
    printk(KERN_INFO "Button 0: Play/Pause\n");
    printk(KERN_INFO "Button 1: Next Track\n");
    printk(KERN_INFO "Button 2: Previous Track\n");
    
    return 0;
}

// Module cleanup
static void __exit cleanup_48khz_audio_player(void)
{
    int i;
    
    printk(KERN_INFO "=== 48kHz cleanup ===\n");
    
    current_state = AUDIO_STOPPED;
    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);
    
    if (audiobase) {
        iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    }
    
    if (audio_wq) {
        flush_workqueue(audio_wq);
        destroy_workqueue(audio_wq);
    }
    
    for (i = 0; i < total_tracks; i++) {
        if (songs[i].file) {
            filp_close(songs[i].file, NULL);
        }
    }
    
    if (lwbridgebase) {
        iowrite32(0x0, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
        iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    }
    
    free_irq(72 + 4, (void*)button_irq_handler);
    
    if (lwbridgebase) iounmap(lwbridgebase);
    if (audiobase) iounmap(audiobase);
    
    printk(KERN_INFO "=== I2S 24-bit Audio Player cleanup ===\n");
}

module_init(initialize_48khz_audio_player);
module_exit(cleanup_48khz_audio_player);